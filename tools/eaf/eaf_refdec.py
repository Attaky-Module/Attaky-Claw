"""Independent EAF reference decoder.

This module mirrors ONLY the chosen decode path from the in-tree C
decoder:

    application/edge_agent/managed_components/espressif2022__esp_emote_gfx/
    src/lib/eaf/gfx_eaf_dec.c

It is the round-trip oracle for ``eaf_encode``. It is written by reading
the C decoder, NOT by inverting the encoder -- if it were derived from
the encoder, a round-trip test would prove nothing. Every step below
cites the gfx_eaf_dec.c line it reimplements.

Scope (the only paths we encode): EAF container -> ``_S`` static frame
header -> ``EAF_ENCODING_RAW`` (tag 0x05) OR ``EAF_ENCODING_RLE``
(tag 0x00) blocks -> 8-bit indexed pixels -> RGB565 via the decoder's
palette math, INCLUDING the transparent sentinel exactly as the real
animation renderer treats it. The RLE path mirrors ``eaf_decode_rle``
(gfx_eaf_dec.c:409-443) byte-for-byte; see ``_decode_rle_block``.

Transparency note (the oracle must equal HARDWARE, not invent its own
rule): the animation widget path is
gfx_anim_prepare_frame -> gfx_anim_render_8bit_pixels (gfx_anim.c),
NOT the standalone eaf_frame_decode() API. In gfx_anim_prepare_frame
(gfx_anim.c:278-284) each palette slot is pre-classified: if
eaf_palette_get_color returns true for the all-zero [0,0,0,0] entry
(gfx_eaf_dec.c:290-292) the cache slot becomes
GFX_PALETTE_SET_TRANSPARENT() (bit 16 marker, gfx_anim.c:31). Then in
gfx_anim_render_8bit_pixels (active #else branch, USE_OLD_PALETTE_CACHE
== 0, gfx_anim.c:24) a pixel whose cache entry has the transparent bit
set is SKIPPED -- ``dst[x]`` is never written (gfx_anim.c:470-481 for
the 4-wide fast path, gfx_anim.c:497-499 for the tail). The destination
frame buffer therefore retains whatever value it already held (the
background). This oracle mirrors that precisely: a transparent index
leaves the pre-existing background value in the decoded buffer; it does
NOT raise.

Pure Python 3 stdlib only.
"""

import struct

# Offset / magic constants (gfx_eaf_dec.h:48-60; FORMAT_NOTES.md section 1).
EAF_FORMAT_OFFSET = 0
EAF_STR_OFFSET = 1
EAF_NUM_OFFSET = 4
EAF_CHECKSUM_OFFSET = 8
EAF_TABLE_LEN = 12
EAF_TABLE_OFFSET = 16
EAF_FORMAT_MAGIC = 0x89
EAF_FORMAT_STR = b"EAF"
AAF_FORMAT_STR = b"AAF"
EAF_MAGIC_HEAD = 0x5A5A
EAF_MAGIC_LEN = 2

EAF_ENCODING_RLE = 0
EAF_ENCODING_RAW = 5


class EafDecodeError(Exception):
    """Raised when the byte stream fails a decoder validity gate."""


def _calculate_checksum(data, start, length):
    """Mirror eaf_calculate_checksum (gfx_eaf_dec.c:24-31).

    Plain unsigned 32-bit sum of bytes (wraps mod 2**32), computed over
    ``data[start : start + length]``. In eaf_init the call is
    ``eaf_calculate_checksum(data + EAF_TABLE_OFFSET, stored_len)``
    (gfx_eaf_dec.c:734).
    """
    checksum = 0
    for i in range(start, start + length):
        checksum += data[i]
    return checksum & 0xFFFFFFFF


def _decode_rle_block(payload, out_size, frame_index, block):
    """Mirror eaf_decode_rle EXACTLY (gfx_eaf_dec.c:409-443).

    ``payload`` is the post-tag block bytes (the C decoder is called
    with ``block_data + 1, block_len - 1`` at gfx_eaf_dec.c:400, so the
    0x00 encoding tag is already stripped here -- same as the RAW path).
    ``out_size`` enters as ``width*block_height`` (gfx_eaf_dec.c:394,
    non-JPEG branch), exactly as the C ``*out_size`` does.

    C source, verbatim shape (gfx_eaf_dec.c:415-442):

        size_t in_pos = 0;
        size_t out_pos = 0;
        while (in_pos + 1 < input_size) {              // :418
            uint8_t repeat_count = input_data[in_pos++]; // :419
            uint8_t repeat_value = input_data[in_pos++]; // :420
            if (out_pos + repeat_count > *out_size) {    // :422
                return ESP_FAIL;                          // :424
            }
            ... write repeat_count copies of repeat_value ... // :427-438
        }
        *out_size = out_pos;                            // :441
        return ESP_OK;

    Subtleties, all taken straight from the C source:
      * Loop guard ``in_pos + 1 < input_size`` (:418): a pair is read
        only if at least TWO bytes remain. A lone trailing odd byte is
        therefore silently dropped (never decoded). The encoder must
        emit an even-length pair stream so no data is lost.
      * Each pair is (count, value) IN THAT ORDER, both uint8 (:419-420);
        ``repeat_count`` is a uint8 so the maximum run a single pair can
        express is 255 -- longer runs MUST be split into multiple pairs.
      * ``repeat_count == 0`` writes nothing (the :428/:435 loops do not
        execute): a valid no-op pair (the C bounds check ``out_pos + 0 >
        *out_size`` is also harmless).
      * The 4-byte fast path (:428-433) then the byte tail (:435-438)
        together write exactly ``repeat_count`` copies of
        ``repeat_value`` -- semantically a plain run; reproduced as such.
      * Overflow gate (:422-425): ``out_pos + repeat_count > *out_size``
        -> ESP_FAIL. Mirrored as EafDecodeError.
      * ``*out_size = out_pos`` (:441): decoded length is whatever was
        produced (the renderer's valid_size clamp, gfx_eaf_dec.c:889-893,
        then governs how much is converted -- same as RAW).

    Returns the decoded index bytes (length == out_pos).
    """
    input_size = len(payload)
    out = bytearray()
    in_pos = 0
    # while (in_pos + 1 < input_size)  -- gfx_eaf_dec.c:418
    while in_pos + 1 < input_size:
        repeat_count = payload[in_pos]      # :419
        in_pos += 1
        repeat_value = payload[in_pos]      # :420
        in_pos += 1
        # if (out_pos + repeat_count > *out_size) -> FAIL  :422-425
        if len(out) + repeat_count > out_size:
            raise EafDecodeError(
                "frame %d block %d RLE decompressed overflow: %d > %d"
                % (frame_index, block, len(out) + repeat_count, out_size)
            )
        # write repeat_count copies of repeat_value  :427-438
        out.extend(bytes([repeat_value]) * repeat_count)
    # *out_size = out_pos  :441 (length is implicit in len(out))
    return bytes(out)


def decode(data, background=0):
    """Decode an EAF file (bytes) to a list of per-frame results.

    Returns a list, one entry per frame, each a dict:

        {
            "format": b"_S",
            "bit_depth": int,
            "width": int,
            "height": int,
            "blocks": int,
            "block_height": int,
            "block_len": [int, ...],          # blocks entries
            "rgb565": [int, ...],             # width*height row-major
            "raw_blocks": [bytes, ...],       # exact on-disk block bytes
                                              # (encoding tag included)
        }

    Plus a top-level container dict is returned alongside via the
    return tuple (container_info, frames).

    ``background``: the pre-existing RGB565 value of the destination
    frame buffer, i.e. what the on-device renderer would leave in place
    for a TRANSPARENT pixel (a pixel whose palette index maps to the
    all-zero [0,0,0,0] sentinel). Mirrors gfx_anim_render_8bit_pixels
    skipping ``dst[x]`` (gfx_anim.c:470-481 / 497-499). Default 0 (the
    frame buffer initialised to all-zero). May be a single int (uniform
    background) or a list of width*height RGB565 values (per-pixel
    background) to model rendering over arbitrary prior contents.
    """
    data = bytes(data)

    # --- eaf_init container parse (gfx_eaf_dec.c:705-765) ---

    # Magic byte (gfx_eaf_dec.c:724).
    if data[EAF_FORMAT_OFFSET] != EAF_FORMAT_MAGIC:
        raise EafDecodeError("bad file format magic")

    # Format string (gfx_eaf_dec.c:726-728): memcmp of 3 bytes at +1.
    format_str = data[EAF_STR_OFFSET:EAF_STR_OFFSET + 3]
    if format_str != EAF_FORMAT_STR and format_str != AAF_FORMAT_STR:
        raise EafDecodeError("bad file format string (expected EAF or AAF)")

    # total_frames read as signed int32 (gfx_eaf_dec.c:730).
    total_frames = struct.unpack_from("<i", data, EAF_NUM_OFFSET)[0]
    # stored_chk / stored_len read as uint32 (gfx_eaf_dec.c:731-732).
    stored_chk = struct.unpack_from("<I", data, EAF_CHECKSUM_OFFSET)[0]
    stored_len = struct.unpack_from("<I", data, EAF_TABLE_LEN)[0]

    # Checksum over data+16 for stored_len bytes (gfx_eaf_dec.c:734-735).
    calculated_chk = _calculate_checksum(data, EAF_TABLE_OFFSET, stored_len)
    if calculated_chk != stored_chk:
        raise EafDecodeError(
            "bad full checksum (calc=%d stored=%d)" % (calculated_chk, stored_chk)
        )

    # Frame table: total_frames packed entries of {u32 frame_size,
    # u32 frame_offset}, #pragma pack(1) so 8 bytes each, starting at
    # EAF_TABLE_OFFSET (gfx_eaf_dec.c:739-746; gfx_eaf_dec.h:69-74).
    table = []
    for i in range(total_frames):
        entry_off = EAF_TABLE_OFFSET + i * 8
        frame_size, frame_offset = struct.unpack_from("<II", data, entry_off)
        table.append((frame_size, frame_offset))

    # frame_mem = data + EAF_TABLE_OFFSET + total_frames*8 + frame_offset
    # (gfx_eaf_dec.c:742). frame_offset is relative to the start of the
    # frame-data region.
    data_region = EAF_TABLE_OFFSET + total_frames * 8

    container_info = {
        "total_frames": total_frames,
        "stored_chk": stored_chk,
        "stored_len": stored_len,
        "format_str": format_str,
        "table": table,
    }

    frames = []
    for i in range(total_frames):
        frame_size, frame_offset = table[i]
        frame_mem = data_region + frame_offset

        # Per-frame 2-byte magic 0x5A5A (gfx_eaf_dec.c:744-745).
        magic = struct.unpack_from("<H", data, frame_mem)[0]
        if magic != EAF_MAGIC_HEAD:
            raise EafDecodeError("bad file magic header (frame %d)" % i)

        # eaf_get_frame_data returns frame_mem + EAF_MAGIC_LEN
        # (gfx_eaf_dec.c:804). The _S parser sees data past the magic.
        fdata = frame_mem + EAF_MAGIC_LEN
        # eaf_get_frame_size returns frame_size - EAF_MAGIC_LEN
        # (gfx_eaf_dec.c:821); validity gate file_size <= 0
        # (gfx_eaf_dec.c:199-201, probe :147-149).
        file_size = frame_size - EAF_MAGIC_LEN
        if file_size <= 0:
            raise EafDecodeError("frame %d invalid size" % i)

        frames.append(_decode_frame(data, fdata, i, background))

    return container_info, frames


def _decode_frame(data, base, frame_index, background=0):
    """Mirror eaf_get_frame_info + eaf_frame_decode for the _S/RAW/8-bit
    path. ``base`` is the absolute offset of the _S stream (= frame blob
    +2, past the 0x5A5A magic)."""

    # --- eaf_get_frame_info (gfx_eaf_dec.c:185-259) ---

    # format[2] at +0 (gfx_eaf_dec.c:206). Byte at +2 never read; version
    # copied from +3 (gfx_eaf_dec.c:210) but value unchecked.
    fmt = data[base:base + 2]
    if fmt != b"_S":
        # _C -> EAF_FORMAT_FLAG (gfx_eaf_dec.c:253-254), consumer rejects
        # it (gfx_anim.c:234). Anything else -> INVALID.
        raise EafDecodeError(
            "frame %d format %r not _S (decoder rejects _C / unknown)"
            % (frame_index, fmt)
        )

    # bit_depth at +9, must be 4/8/24 (gfx_eaf_dec.c:212-217).
    bit_depth = data[base + 9]
    if bit_depth not in (4, 8, 24):
        raise EafDecodeError("frame %d invalid bit depth %d" % (frame_index, bit_depth))

    # width/height/blocks/block_height as uint16 LE at +10/+12/+14/+16
    # (gfx_eaf_dec.c:219-222).
    width = struct.unpack_from("<H", data, base + 10)[0]
    height = struct.unpack_from("<H", data, base + 12)[0]
    blocks = struct.unpack_from("<H", data, base + 14)[0]
    block_height = struct.unpack_from("<H", data, base + 16)[0]

    # eaf_probe_frame_info rejects any of the four == 0
    # (gfx_eaf_dec.c:173-175). eaf_frame_decode requires a valid header.
    if width == 0 or height == 0 or blocks == 0 or block_height == 0:
        raise EafDecodeError(
            "frame %d has a zero dimension (w=%d h=%d b=%d bh=%d)"
            % (frame_index, width, height, blocks, block_height)
        )

    # block_len[i] = *(uint32_t*)(file_data + 18 + i*4)
    # (gfx_eaf_dec.c:230-232).
    block_len = []
    for i in range(blocks):
        block_len.append(struct.unpack_from("<I", data, base + 18 + i * 4)[0])

    # num_colors = 1 << bit_depth (gfx_eaf_dec.c:234). For 24-bit,
    # num_colors=0 / palette=NULL (gfx_eaf_dec.c:236-238).
    if bit_depth == 24:
        num_colors = 0
        palette = None
    else:
        num_colors = 1 << bit_depth
        # palette memcpy'd from file_data + 18 + blocks*4, num_colors*4
        # bytes (gfx_eaf_dec.c:240,248).
        pal_off = base + 18 + blocks * 4
        palette = data[pal_off:pal_off + num_colors * 4]

    # data_offset = 18 + blocks*4 + num_colors*4 (gfx_eaf_dec.c:250).
    data_offset = 18 + blocks * 4 + num_colors * 4

    # --- eaf_frame_decode (gfx_eaf_dec.c:828-923) ---

    # block_size = width*block_height; *2 for 24-bit (gfx_eaf_dec.c:854-855).
    block_size = width * block_height
    if bit_depth == 24:
        block_size *= 2

    # eaf_calculate_offsets (gfx_eaf_dec.c:273-279):
    # offsets[0] = data_offset; offsets[i] = offsets[i-1] + block_len[i-1].
    offsets = [0] * blocks
    offsets[0] = data_offset
    for i in range(1, blocks):
        offsets[i] = offsets[i - 1] + block_len[i - 1]

    # frame_buffer holds width*height uint16 RGB565 (allocated by caller
    # gfx_anim.c). On device its prior contents are the background that
    # transparent (skipped) pixels keep. Model that exactly: seed the
    # buffer with the caller-supplied background (uniform int or a
    # per-pixel list), then only OPAQUE pixels overwrite their slot --
    # mirroring gfx_anim_render_8bit_pixels (gfx_anim.c:470-481/497-499)
    # which never writes dst[x] for a transparent index.
    npix = width * height
    if isinstance(background, (list, tuple)):
        if len(background) != npix:
            raise EafDecodeError(
                "frame %d background has %d entries, expected %d"
                % (frame_index, len(background), npix))
        rgb565 = [v & 0xFFFF for v in background]
    else:
        rgb565 = [background & 0xFFFF] * npix

    if bit_depth != 8:
        # 4-bit explicitly unsupported (gfx_eaf_dec.c:911-912); 24-bit
        # is a raw RGB565 memcpy (gfx_eaf_dec.c:913-914). The encoder
        # only emits 8-bit, so the oracle only needs the 8-bit path.
        raise EafDecodeError(
            "refdec only mirrors the 8-bit render path (frame %d depth %d)"
            % (frame_index, bit_depth)
        )

    raw_blocks = []
    for block in range(blocks):
        # block_data = frame_data + offsets[block]; block_len entry
        # (gfx_eaf_dec.c:877-878).
        blk_start = base + offsets[block]
        blen = block_len[block]
        blk = data[blk_start:blk_start + blen]
        raw_blocks.append(blk)  # exact on-disk block bytes (tag included)

        # --- eaf_decode_block (gfx_eaf_dec.c:369-407) ---
        encoding_type = blk[0]  # first byte = encoding tag (:372).
        if encoding_type >= 6:  # EAF_ENCODING_MAX array bound (:378).
            raise EafDecodeError("frame %d unknown encoding %02X" % (frame_index, encoding_type))
        if encoding_type not in (EAF_ENCODING_RAW, EAF_ENCODING_RLE):
            # Only RAW + RLE are in the chosen path; the oracle
            # deliberately does not reimplement Huffman/JPEG/heatshrink.
            raise EafDecodeError(
                "frame %d encoding %d not RAW/RLE (oracle scope)"
                % (frame_index, encoding_type)
            )

        # out_size enters as width*block_height (non-JPEG branch,
        # gfx_eaf_dec.c:394/397). decoder called with block_data+1,
        # block_len-1 (gfx_eaf_dec.c:400) -- tag byte stripped (for
        # BOTH RAW and RLE; the dispatch is identical, only the
        # registered callback differs, gfx_eaf_dec.c:355/364).
        out_size = width * block_height
        payload = blk[1:blen]

        if encoding_type == EAF_ENCODING_RLE:
            # --- eaf_decode_rle (gfx_eaf_dec.c:409-443) ---
            decoded = _decode_rle_block(payload, out_size,
                                        frame_index, block)
        else:
            # --- eaf_decode_raw (gfx_eaf_dec.c:445-466) ---
            input_size = len(payload)
            if out_size < input_size:  # guard at gfx_eaf_dec.c:456.
                raise EafDecodeError(
                    "frame %d block %d RAW payload too big: %d > %d"
                    % (frame_index, block, input_size, out_size)
                )
            # memcpy(out, in, input_size); *out_size = input_size
            # (:461-464).
            decoded = payload  # decode_buffer's first input_size bytes.

        # --- per-block RGB565 mapping (gfx_eaf_dec.c:886-910) ---
        # block_buffer = frame_buffer + block*block_height*width.
        base_pixel = block * block_height * width

        # valid_size clamps the final partial strip (gfx_eaf_dec.c:889-893).
        if (block + 1) * block_height > height:
            valid_size = (height - block * block_height) * width
        else:
            valid_size = block_size  # == width*block_height for 8-bit.

        for i in range(valid_size):
            index = decoded[i]
            color = _palette_get_color(palette, index)
            # color is None for the [0,0,0,0] transparent sentinel:
            # gfx_anim_render_8bit_pixels skips the write, so the
            # background value already in the buffer is preserved
            # (gfx_anim.c:470-481 / 497-499). Only opaque pixels write.
            if color is not None:
                rgb565[base_pixel + i] = color

    return {
        "format": fmt,
        "bit_depth": bit_depth,
        "width": width,
        "height": height,
        "blocks": blocks,
        "block_height": block_height,
        "block_len": block_len,
        "rgb565": rgb565,
        "raw_blocks": raw_blocks,
    }


def _palette_get_color(palette, color_index):
    """Mirror eaf_palette_get_color (gfx_eaf_dec.c:285-303), swap_bytes
    = False (display-endianness concern, independent of the file),
    classified the way the animation renderer consumes its return value
    (gfx_anim_prepare_frame, gfx_anim.c:278-284).

    Returns:
      * None  -- the entry is the all-zero [0,0,0,0] transparent
        sentinel. gfx_eaf_dec.c:290-292 returns true; the renderer marks
        the cache slot GFX_PALETTE_SET_TRANSPARENT() (gfx_anim.c:280)
        and gfx_anim_render_8bit_pixels then SKIPS the pixel write
        entirely (gfx_anim.c:470-481 / 497-499), leaving the background
        in place. The caller treats None as "do not write this pixel".
      * int   -- the 16-bit RGB565 value the renderer caches and writes
        for an opaque entry (gfx_anim.c:282).
    """
    c = palette[color_index * 4: color_index * 4 + 4]

    # Fully-transparent sentinel: all four bytes 0 (gfx_eaf_dec.c:290-292
    # returns true). Hardware does NOT fail here -- the animation
    # renderer simply does not write the pixel (gfx_anim.c:470-499).
    if c[0] == 0 and c[1] == 0 and c[2] == 0 and c[3] == 0:
        return None

    # RGB565: R=c[2], G=c[1], B=c[0] (gfx_eaf_dec.c:294-300).
    rgb565 = (
        ((c[2] & 0xF8) << 8)
        | ((c[1] & 0xFC) << 3)
        | ((c[0] & 0xF8) >> 3)
    ) & 0xFFFF
    return rgb565
