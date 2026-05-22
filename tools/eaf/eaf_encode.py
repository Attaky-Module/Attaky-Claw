"""Offline EAF encoder (host tool).

Emits the byte-exact ``EAF`` container + per-frame ``_S`` static-image
header + ``EAF_ENCODING_RLE`` (tag 0x00, default) or
``EAF_ENCODING_RAW`` (tag 0x05) blocks + 8-bit indexed palette, exactly
per ``tools/eaf/FORMAT_NOTES.md`` (itself derived from the in-tree
decoder ``gfx_eaf_dec.c``).

Deliberate scope (the only paths the on-device decoder + renderer
actually accept):

  * container magic 0x89 + "EAF"
  * ``_S`` frames only -- never ``_C`` (gfx_anim.c hard-rejects ``_C``)
  * ``EAF_ENCODING_RLE`` blocks, tag byte 0x00 (the default; what every
    shipping animation asset uses -- compresses the flat
    opaque-black background of a status disc massively), OR
    ``EAF_ENCODING_RAW`` blocks, tag byte 0x05 (uncompressed, retained
    and still valid; selectable via ``encoding=EAF_ENCODING_RAW``)
  * 8-bit indexed pixels, 256-entry [B,G,R,A] palette

The per-block encoding is the ONLY thing ``encoding`` changes; the
container / ``_S`` header / palette / frame table / checksum logic is
identical for both (only the per-block stored length in the block-length
table varies, since RLE blocks compress to fewer bytes).

Pure Python 3 stdlib only. RLE only (no Huffman/JPEG/heatshrink).

Tiling constraint (documented):
  Blocks are horizontal full-width strips of ``block_height`` rows,
  stored contiguously. ``blocks = ceil(height / block_height)``.
  ``block_height`` need NOT divide ``height``; the final strip is
  padded with index 0 up to a full ``width*block_height`` payload (the
  decoder only renders ``valid_size`` rows of the last strip, but the
  RAW payload on disk must still be exactly ``width*block_height``
  bytes -- see FORMAT_NOTES.md section 2 / gfx_eaf_dec.c:889-893,456).
"""

import struct

EAF_FORMAT_MAGIC = 0x89
EAF_FORMAT_STR = b"EAF"
EAF_MAGIC_HEAD = 0x5A5A
EAF_ENCODING_RLE = 0x00
EAF_ENCODING_RAW = 0x05

_BIT_DEPTH = 8
_NUM_COLORS = 1 << _BIT_DEPTH  # 256 (gfx_eaf_dec.c:234)


def _rle_encode_strip(strip):
    """Run-length encode one full ``width*block_height`` index strip into
    the exact (count, value) pair stream the in-tree C decoder
    ``eaf_decode_rle`` consumes (gfx_eaf_dec.c:409-443).

    Decoder contract this MUST satisfy (every rule cited to the C
    source, the format oracle):

      * The decoder loop is ``while (in_pos + 1 < input_size)`` with
        ``repeat_count = input[in_pos++]; repeat_value = input[in_pos++]``
        (gfx_eaf_dec.c:418-420): the stream is a flat sequence of
        ``(count, value)`` byte pairs, count first. We emit exactly that.
      * ``repeat_count`` is a ``uint8`` (gfx_eaf_dec.c:419) so one pair
        expresses at most 255 copies. A run longer than 255 is SPLIT
        into consecutive pairs each <= 255 of the same value
        (e.g. 600 -> 255,255,90). The decoder simply appends each
        pair's expansion, so split runs reconstruct identically.
      * Expanding every pair yields exactly ``len(strip)`` bytes ==
        ``width*block_height``. ``eaf_decode_block`` passes
        ``*out_size = width*block_height`` (gfx_eaf_dec.c:394) and the
        bounds check ``out_pos + repeat_count > *out_size`` (:422) fails
        the decode if we ever over-produce; producing EXACTLY the strip
        keeps the renderer's ``valid_size`` clamp (:889-893) behaving
        identically to the RAW path.
      * The pair stream length is always even, so the decoder's
        ``in_pos + 1 < input_size`` guard (:418) -- which would silently
        drop a lone trailing byte -- never discards data.
      * ``repeat_count == 0`` would be a valid no-op pair for the
        decoder, but we never emit one (every run has length >= 1), so
        the stream is minimal.

    Returns: a ``bytes`` of (count, value) pairs (even length).
    """
    pairs = bytearray()
    n = len(strip)
    i = 0
    while i < n:
        val = strip[i]
        j = i + 1
        while j < n and strip[j] == val:
            j += 1
        run = j - i
        # Split runs > 255 into successive <=255 pairs of the same
        # value (gfx_eaf_dec.c:419 repeat_count is uint8).
        while run > 0:
            chunk = run if run < 255 else 255
            pairs.append(chunk)   # count  (gfx_eaf_dec.c:419)
            pairs.append(val)     # value  (gfx_eaf_dec.c:420)
            run -= chunk
        i = j
    return bytes(pairs)


def encode(frames, width, height, palette, block_height=None,
           encoding=EAF_ENCODING_RLE):
    """Encode ``frames`` into an EAF byte string.

    Args:
      frames: list of frames; each frame is a flat sequence of
        ``width*height`` 8-bit palette indices, row-major
        (index i -> row i//width, col i%width).
      width, height: image dimensions in pixels (both > 0, <= 65535).
      palette: 256*4 bytes, entry ``i`` = ``[B, G, R, A]`` at
        ``palette[i*4 : i*4+4]``. No palette index actually used by any
        frame may map to the all-zero ``[0,0,0,0]`` entry: on real
        hardware that entry is the transparent sentinel
        (gfx_eaf_dec.c:290-292 returns true; gfx_anim.c:278-284 marks
        the cache slot transparent and gfx_anim.c:470-499 then SKIPS
        those pixels), so a caller intending opaque black would
        silently get transparent holes. Use opaque black
        ``[0,0,0,255]`` for a real black pixel. Unused palette slots
        may stay ``[0,0,0,0]`` -- the table is a fixed 256 entries and
        the decoder only consults indices that appear in pixel data.
      block_height: rows per strip-block. Default = ``height`` (single
        block). Need not divide ``height``.

    Returns:
      bytes: the complete EAF file.
    """
    nframes = len(frames)
    if nframes <= 0:
        raise ValueError("need at least one frame")
    if not (0 < width <= 0xFFFF) or not (0 < height <= 0xFFFF):
        raise ValueError("width/height must be in 1..65535")
    if len(palette) != _NUM_COLORS * 4:
        raise ValueError("palette must be exactly %d bytes" % (_NUM_COLORS * 4))

    if encoding not in (EAF_ENCODING_RAW, EAF_ENCODING_RLE):
        raise ValueError("unsupported encoding 0x%02X" % encoding)

    if block_height is None:
        block_height = height
    if not (0 < block_height <= 0xFFFF):
        raise ValueError("block_height must be in 1..65535")

    # blocks = ceil(height / block_height); guarantees
    # blocks*block_height >= height (full coverage, gfx_eaf_dec.c needs
    # all four header dims > 0 -- :173-175).
    blocks = (height + block_height - 1) // block_height
    if blocks > 0xFFFF:
        raise ValueError("too many blocks for uint16 field")

    strip_pixels = width * block_height
    expected_px = width * height

    # Encode-time transparency guard. On real hardware a palette entry
    # whose four bytes are all zero ([B,G,R,A] == [0,0,0,0]) is the
    # transparent sentinel: eaf_palette_get_color returns true
    # (gfx_eaf_dec.c:290-292), gfx_anim_prepare_frame then sets that
    # cache slot to GFX_PALETTE_SET_TRANSPARENT() (gfx_anim.c:278-284),
    # and gfx_anim_render_8bit_pixels SKIPS every pixel using such an
    # index (gfx_anim.c:470-481 / 497-499) -- the destination keeps its
    # prior background value. A caller intending opaque black would get
    # silent transparent holes. So: for every frame, collect the set of
    # palette indices its pixels actually use and reject the encode if
    # any used index's entry is all-zero. Unused slots may stay
    # [0,0,0,0] (the table is a fixed 256 entries; the decoder only
    # consults indices that appear in pixel data).
    for fi, frame in enumerate(frames):
        if len(frame) != expected_px:
            raise ValueError(
                "frame %d has %d pixels, expected %d"
                % (fi, len(frame), expected_px))
        used = set()
        for idx in frame:
            if not (0 <= idx <= 255):
                raise ValueError(
                    "frame %d pixel value %d out of 0..255" % (fi, idx))
            used.add(idx)
        for idx in sorted(used):
            entry = palette[idx * 4: idx * 4 + 4]
            if entry[0] == 0 and entry[1] == 0 and entry[2] == 0 and entry[3] == 0:
                raise ValueError(
                    "frame %d uses palette index %d whose entry is the "
                    "all-zero [0,0,0,0] transparent sentinel; on hardware "
                    "those pixels render TRANSPARENT (not black). Use "
                    "opaque black [0,0,0,255] for a real black pixel "
                    "(gfx_eaf_dec.c:290-292, gfx_anim.c:470-499)."
                    % (fi, idx))

    # data_offset is a uint16 in the decoder (gfx_eaf_dec.h:131); guard.
    data_offset = 18 + blocks * 4 + _NUM_COLORS * 4
    if data_offset >= 65536:
        raise ValueError("_S header+table+palette exceeds uint16 data_offset")

    # --- Build each frame's _S stream + 0x5A5A blob ---
    frame_blobs = []
    for fi, frame in enumerate(frames):
        if len(frame) != expected_px:
            raise ValueError(
                "frame %d has %d pixels, expected %d" % (fi, len(frame), expected_px))

        # Build every block's full width*block_height index strip first
        # (encoding-independent). The final strip is padded with index 0
        # up to a full strip: the decoder only renders valid rows
        # (gfx_eaf_dec.c:889-893), but both eaf_decode_raw (:456) and
        # eaf_decode_rle (:422, *out_size = width*block_height) require
        # the decoded payload to be exactly width*block_height.
        block_payloads = []  # one (tag-prefixed) bytes per block
        for b in range(blocks):
            row0 = b * block_height
            strip = bytearray(strip_pixels)  # zero-padded
            for r in range(block_height):
                src_row = row0 + r
                if src_row >= height:
                    break  # remaining rows stay index 0 (padding)
                src = src_row * width
                dst = r * width
                for c in range(width):
                    idx = frame[src + c]
                    if not (0 <= idx <= 255):
                        raise ValueError(
                            "frame %d pixel value %d out of 0..255" % (fi, idx))
                    strip[dst + c] = idx

            if encoding == EAF_ENCODING_RLE:
                # 0x00 tag + (count,value) pair stream. eaf_decode_block
                # strips the tag (block_data+1, block_len-1,
                # gfx_eaf_dec.c:400) then eaf_decode_rle expands the
                # pairs back to exactly strip_pixels bytes.
                payload = (struct.pack("<B", EAF_ENCODING_RLE)
                           + _rle_encode_strip(strip))
            else:
                # 0x05 tag + raw width*block_height index bytes
                # (FORMAT_NOTES.md section 4; eaf_decode_raw memcpy,
                # gfx_eaf_dec.c:445-466).
                payload = struct.pack("<B", EAF_ENCODING_RAW) + bytes(strip)
            block_payloads.append(payload)

        # _S header (FORMAT_NOTES.md section 5; gfx_eaf_dec.c:206-250).
        hdr = bytearray()
        hdr += b"_S"                       # +0 format[2]
        hdr += b"\x00"                     # +2 unused separator (emit 0)
        hdr += b"\x00" * 6                 # +3 version[6] (unchecked)
        hdr += struct.pack("<B", _BIT_DEPTH)  # +9 bit_depth = 8
        hdr += struct.pack("<H", width)    # +10 width  u16 LE
        hdr += struct.pack("<H", height)   # +12 height u16 LE
        hdr += struct.pack("<H", blocks)   # +14 blocks u16 LE
        hdr += struct.pack("<H", block_height)  # +16 block_height u16 LE

        # Block-length table: blocks * u32 LE. Each entry is the EXACT
        # stored size of that block including its 1-byte encoding tag --
        # the value the decoder reads at gfx_eaf_dec.c:231 and uses both
        # as block_len in eaf_decode_block (:400, passes block_len-1 to
        # the decoder) and to chain offsets in eaf_calculate_offsets
        # (:273-279). For RAW this is the constant 1+width*block_height;
        # for RLE it varies per block with the compressed pair count.
        for payload in block_payloads:
            hdr += struct.pack("<I", len(payload))

        # Palette: 256 * 4 bytes, [B,G,R,A] (gfx_eaf_dec.c:240,248).
        hdr += bytes(palette)

        # Block payloads, concatenated in declared order at data_offset
        # (gfx_eaf_dec.c:273-278 chains them by block_len).
        body = bytearray()
        for payload in block_payloads:
            body += payload

        s_stream = bytes(hdr) + bytes(body)
        # Frame blob = 0x5A 0x5A + _S stream (gfx_eaf_dec.c:744;
        # frame_size in table includes these 2 magic bytes -- :821).
        blob = struct.pack("<H", EAF_MAGIC_HEAD) + s_stream
        frame_blobs.append(blob)

    # --- Frame table + data region ---
    # frame_offset is relative to the start of the frame-data region
    # (= 16 + nframes*8), gfx_eaf_dec.c:742. Frames are concatenated.
    table = bytearray()
    data_region = bytearray()
    running_offset = 0
    for blob in frame_blobs:
        frame_size = len(blob)            # includes the 2 magic bytes.
        table += struct.pack("<II", frame_size, running_offset)
        data_region += blob
        running_offset += frame_size

    # Region that the checksum covers = table + data (everything from
    # file offset 16 onward), gfx_eaf_dec.c:734.
    checksummed = bytes(table) + bytes(data_region)
    stored_len = len(checksummed)
    stored_chk = sum(checksummed) & 0xFFFFFFFF  # eaf_calculate_checksum.

    # --- Container header (16 bytes) ---
    header = bytearray()
    header += struct.pack("<B", EAF_FORMAT_MAGIC)  # +0 magic 0x89
    header += EAF_FORMAT_STR                        # +1 "EAF"
    header += struct.pack("<i", nframes)            # +4 total_frames (i32)
    header += struct.pack("<I", stored_chk)         # +8 checksum u32
    header += struct.pack("<I", stored_len)         # +12 length u32

    return bytes(header) + checksummed
