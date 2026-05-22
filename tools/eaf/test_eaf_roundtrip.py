"""Host round-trip test: eaf_encode -> eaf_refdec, pixel-exact.

Run directly:

    python3 tools/eaf/test_eaf_roundtrip.py

Self-reports pass/fail and exits nonzero on any failure. Pure stdlib.

The oracle (eaf_refdec) is an independent reimplementation of the C
decoder path, NOT an inversion of the encoder, so an exact match proves
the encoder produces bytes the on-device decoder will decode correctly.
"""

import os
import struct
import sys
import traceback

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))

import eaf_encode
import eaf_refdec


# ---------------------------------------------------------------------------
# Helpers
# ---------------------------------------------------------------------------

def expected_rgb565(b, g, r):
    """The exact 565 quantisation the C decoder applies
    (gfx_eaf_dec.c:299-300): R=c[2], G=c[1], B=c[0]."""
    return (((r & 0xF8) << 8) | ((g & 0xFC) << 3) | ((b & 0xF8) >> 3)) & 0xFFFF


def make_palette(colors):
    """Build a 256-entry [B,G,R,A] palette. ``colors`` is a dict
    index -> (B,G,R,A). Unspecified entries default to opaque black
    [0,0,0,255] (NOT [0,0,0,0], which is the transparent sentinel)."""
    pal = bytearray(256 * 4)
    for i in range(256):
        pal[i * 4 + 3] = 0xFF  # default opaque black
    for idx, (bch, gch, rch, ach) in colors.items():
        pal[idx * 4 + 0] = bch
        pal[idx * 4 + 1] = gch
        pal[idx * 4 + 2] = rch
        pal[idx * 4 + 3] = ach
    return bytes(pal)


def render_expected(frame_indices, width, height, palette, background=0):
    """Compute the RGB565 buffer the on-device renderer must produce for
    an indexed frame, independent of block tiling (full image,
    row-major). Mirrors gfx_anim_render_8bit_pixels: an index whose
    palette entry is the all-zero [0,0,0,0] transparent sentinel is
    SKIPPED, so the background value is kept (gfx_anim.c:470-499);
    opaque entries are quantised to RGB565 (gfx_eaf_dec.c:299-300)."""
    npix = len(frame_indices)
    if isinstance(background, (list, tuple)):
        out = [v & 0xFFFF for v in background]
    else:
        out = [background & 0xFFFF] * npix
    for i, px in enumerate(frame_indices):
        c = palette[px * 4: px * 4 + 4]
        if c[0] == 0 and c[1] == 0 and c[2] == 0 and c[3] == 0:
            continue  # transparent: keep background, no write
        out[i] = expected_rgb565(c[0], c[1], c[2])
    return out


# ---------------------------------------------------------------------------
# Test cases
# ---------------------------------------------------------------------------

FAILURES = []


def check(cond, msg):
    if not cond:
        FAILURES.append(msg)
        print("  FAIL: " + msg)
    return cond


def run_case(name, frames_idx, width, height, palette, block_height,
             background=0, encoding=eaf_encode.EAF_ENCODING_RAW):
    """frames_idx: list of frames, each a flat list of width*height
    palette indices (row-major). ``background`` models the destination
    frame buffer's prior contents (uniform int or per-pixel list) that
    transparent pixels keep -- passed to refdec and to render_expected
    identically so the oracle and the expected value agree on the
    hardware skip semantics. ``encoding`` selects the per-block encoding
    (RAW tag 0x05 or RLE tag 0x00); both must round-trip pixel-exact."""
    print("[case] %s (w=%d h=%d nframes=%d block_height=%d enc=0x%02X)"
          % (name, width, height, len(frames_idx), block_height, encoding))
    try:
        blob = eaf_encode.encode(frames_idx, width, height, palette,
                                 block_height=block_height,
                                 encoding=encoding)
    except Exception as exc:  # noqa: BLE001
        FAILURES.append("%s: encode() raised %r" % (name, exc))
        traceback.print_exc()
        return

    try:
        container, decoded = eaf_refdec.decode(blob, background=background)
    except Exception as exc:  # noqa: BLE001
        FAILURES.append("%s: refdec.decode() raised %r" % (name, exc))
        traceback.print_exc()
        return

    # Container-level header checks.
    check(container["total_frames"] == len(frames_idx),
          "%s: frame count %d != %d" % (name, container["total_frames"], len(frames_idx)))

    # Recompute the checksum the way the decoder gates it and confirm
    # the encoder's stored value matches (decode() already enforces this,
    # but assert the field explicitly per the task).
    expected_len = len(blob) - 16
    check(container["stored_len"] == expected_len,
          "%s: stored_len %d != %d" % (name, container["stored_len"], expected_len))
    chk = sum(blob[16:]) & 0xFFFFFFFF
    check(container["stored_chk"] == chk,
          "%s: stored_chk %d != recomputed %d" % (name, container["stored_chk"], chk))

    n_blocks_expected = (height + block_height - 1) // block_height

    for fi, frame_idx in enumerate(frames_idx):
        d = decoded[fi]
        check(d["format"] == b"_S", "%s f%d: format %r" % (name, fi, d["format"]))
        check(d["bit_depth"] == 8, "%s f%d: bit_depth %d" % (name, fi, d["bit_depth"]))
        check(d["width"] == width, "%s f%d: width %d" % (name, fi, d["width"]))
        check(d["height"] == height, "%s f%d: height %d" % (name, fi, d["height"]))
        check(d["block_height"] == block_height,
              "%s f%d: block_height %d" % (name, fi, d["block_height"]))
        check(d["blocks"] == n_blocks_expected,
              "%s f%d: blocks %d != %d" % (name, fi, d["blocks"], n_blocks_expected))
        if encoding == eaf_encode.EAF_ENCODING_RAW:
            # Each RAW block stored length must be 1 + width*block_height.
            for bi, bl in enumerate(d["block_len"]):
                check(bl == 1 + width * block_height,
                      "%s f%d b%d: block_len %d != %d"
                      % (name, fi, bi, bl, 1 + width * block_height))
        else:
            # RLE: each block on disk must be tag 0x00 followed by
            # (count, index) pairs whose counts sum to exactly
            # width*block_height, with no run > 255 and no trailing
            # odd byte (the C decoder's loop drops a lone last byte).
            strip = width * block_height
            for bi, raw in enumerate(d["raw_blocks"]):
                if not check(raw[0] == eaf_encode.EAF_ENCODING_RLE,
                             "%s f%d b%d: RLE tag 0x%02X != 0x00"
                             % (name, fi, bi, raw[0])):
                    continue
                pairs = raw[1:]
                if not check(len(pairs) % 2 == 0,
                             "%s f%d b%d: RLE pair stream has odd length %d"
                             % (name, fi, bi, len(pairs))):
                    continue
                total = 0
                bad_run = False
                for pi in range(0, len(pairs), 2):
                    cnt = pairs[pi]
                    if cnt > 255:
                        bad_run = True
                    total += cnt
                check(not bad_run,
                      "%s f%d b%d: RLE run count exceeds 255"
                      % (name, fi, bi))
                check(total == strip,
                      "%s f%d b%d: RLE counts sum %d != strip %d"
                      % (name, fi, bi, total, strip))

        want = render_expected(frame_idx, width, height, palette,
                               background=background)
        got = d["rgb565"]
        check(len(got) == len(want),
              "%s f%d: pixel count %d != %d" % (name, fi, len(got), len(want)))
        if got != want:
            # Report first mismatch only.
            for i, (a, b) in enumerate(zip(got, want)):
                if a != b:
                    FAILURES.append(
                        "%s f%d: pixel %d (row=%d col=%d) got 0x%04X want 0x%04X"
                        % (name, fi, i, i // width, i % width, a, b))
                    print("  FAIL: " + FAILURES[-1])
                    break

    if not any(f.startswith(name + ":") or f.startswith(name + " ") for f in FAILURES):
        print("  ok")


def gradient_frames():
    """3 frames, 16x16, 8-bit indexed. Palette includes pure opaque
    black, pure white, a red/green/blue, and a 16-step grey gradient."""
    width = height = 16
    colors = {
        0: (0, 0, 0, 0xFF),        # opaque black (A=0xFF, NOT 0,0,0,0)
        1: (0xFF, 0xFF, 0xFF, 0xFF),  # white
        2: (0x00, 0x00, 0xFF, 0xFF),  # red   (B,G,R,A) -> R=0xFF
        3: (0x00, 0xFF, 0x00, 0xFF),  # green
        4: (0xFF, 0x00, 0x00, 0xFF),  # blue
    }
    # 16-step grey gradient at indices 16..31.
    for s in range(16):
        v = (s * 255) // 15
        colors[16 + s] = (v, v, v, 0xFF)
    palette = make_palette(colors)

    frames = []
    # Frame 0: horizontal grey gradient bands.
    f0 = []
    for row in range(height):
        for col in range(width):
            f0.append(16 + (col % 16))
    frames.append(f0)
    # Frame 1: checkerboard of black/white.
    f1 = []
    for row in range(height):
        for col in range(width):
            f1.append(1 if (row + col) % 2 == 0 else 0)
    frames.append(f1)
    # Frame 2: RGB quadrants + a black corner.
    f2 = []
    for row in range(height):
        for col in range(width):
            if row < 8 and col < 8:
                f2.append(2)
            elif row < 8:
                f2.append(3)
            elif col < 8:
                f2.append(4)
            else:
                f2.append(0)
    frames.append(f2)
    return frames, width, height, palette


# GOLDEN RGB565 vectors. Each (B, G, R, A) -> expected uint16, the
# value computed BY HAND from the C source formula at
# gfx_eaf_dec.c:299-300:  ((R&0xF8)<<8)|((G&0xFC)<<3)|((B&0xF8)>>3).
# This anchors the quantisation to the C source independently of
# eaf_refdec / expected_rgb565 (which share a formula). Worked by hand:
#   white  [255,255,255,255]: F800|07E0|001F = 0xFFFF
#   red    [  0,  0,255,255]: F800|0000|0000 = 0xF800
#   green  [  0,255,  0,255]: 0000|07E0|0000 = 0x07E0
#   blue   [255,  0,  0,255]: 0000|0000|001F = 0x001F
#   oblack [  0,  0,  0,255]: 0000|0000|0000 = 0x0000  (A=255 -> opaque)
#   grey   [128,128,128,255]: (0x80<<8)|(0x80<<3)|(0x80>>3)
#                            = 8000|0400|0010 = 0x8410
GOLDEN = [
    ((255, 255, 255, 255), 0xFFFF),  # pure white
    ((0,   0,   255, 255), 0xF800),  # pure red
    ((0,   255, 0,   255), 0x07E0),  # pure green
    ((255, 0,   0,   255), 0x001F),  # pure blue
    ((0,   0,   0,   255), 0x0000),  # opaque black
    ((128, 128, 128, 255), 0x8410),  # mid grey
]


def golden_case():
    """Hardcoded golden vectors. Build a 1x6 frame whose 6 pixels are
    the 6 golden colours (indices 1..6), encode -> refdec, and assert
    each decoded pixel equals the BY-HAND C-formula constant. Proves the
    encoder->refdec path reproduces gfx_eaf_dec.c:299-300 exactly,
    anchored to the C source, not to refdec's own formula."""
    name = "golden_rgb565"
    print("[case] %s (%d hand-computed C-formula vectors)" % (name, len(GOLDEN)))
    width, height = len(GOLDEN), 1
    colors = {}
    for i, ((b, g, r, a), _exp) in enumerate(GOLDEN):
        colors[i + 1] = (b, g, r, a)  # index 0 stays default opaque black
    palette = make_palette(colors)
    frame = [i + 1 for i in range(len(GOLDEN))]

    try:
        blob = eaf_encode.encode([frame], width, height, palette,
                                 block_height=height)
        _container, decoded = eaf_refdec.decode(blob)
    except Exception as exc:  # noqa: BLE001
        FAILURES.append("%s: pipeline raised %r" % (name, exc))
        traceback.print_exc()
        return

    got = decoded[0]["rgb565"]
    ok = True
    for i, ((b, g, r, a), expected) in enumerate(GOLDEN):
        if not check(got[i] == expected,
                     "%s: pixel %d [B=%d G=%d R=%d A=%d] got 0x%04X want 0x%04X"
                     % (name, i, b, g, r, a, got[i], expected)):
            ok = False
    if ok:
        print("  ok")


def encode_rejects_transparent_used_index():
    """encode() MUST raise when a USED palette index maps to the
    all-zero [0,0,0,0] transparent sentinel (would render as silent
    transparent holes on hardware, gfx_anim.c:470-499). An UNUSED
    [0,0,0,0] slot must NOT raise."""
    name = "encode_rejects_transparent"
    print("[case] %s" % name)

    # Palette: index 5 is the all-zero sentinel; everything else opaque.
    pal = bytearray(256 * 4)
    for i in range(256):
        pal[i * 4 + 3] = 0xFF
    pal[5 * 4 + 0] = 0
    pal[5 * 4 + 1] = 0
    pal[5 * 4 + 2] = 0
    pal[5 * 4 + 3] = 0  # index 5 -> [0,0,0,0]
    pal = bytes(pal)

    # (a) frame USES index 5 -> must raise ValueError naming index 5.
    bad_frame = [5] * (4 * 4)
    try:
        eaf_encode.encode([bad_frame], 4, 4, pal, block_height=4)
        FAILURES.append("%s: encode() did NOT raise for used [0,0,0,0] index" % name)
        print("  FAIL: " + FAILURES[-1])
        return
    except ValueError as exc:
        if "5" not in str(exc):
            FAILURES.append("%s: ValueError did not name index 5: %s" % (name, exc))
            print("  FAIL: " + FAILURES[-1])
            return
    except Exception as exc:  # noqa: BLE001
        FAILURES.append("%s: wrong exception type %r" % (name, exc))
        traceback.print_exc()
        return

    # (b) same palette, frame does NOT use index 5 -> must NOT raise.
    good_frame = [0] * (4 * 4)
    try:
        blob = eaf_encode.encode([good_frame], 4, 4, pal, block_height=4)
        eaf_refdec.decode(blob)
    except Exception as exc:  # noqa: BLE001
        FAILURES.append("%s: encode() wrongly raised for UNUSED [0,0,0,0]: %r"
                        % (name, exc))
        traceback.print_exc()
        return
    print("  ok")


def transparency_case():
    """A frame using a [0,0,0,0] index over a known non-zero background.
    The oracle must reproduce gfx_anim.c's behavior: transparent pixels
    are SKIPPED, keeping the background; opaque pixels overwrite. We
    drive refdec directly (encode() intentionally refuses a used
    [0,0,0,0] index, so this exercises the renderer-fidelity model)."""
    name = "transparency_over_bg"
    print("[case] %s" % name)
    width, height = 4, 2
    # index 0 = opaque red; index 1 = transparent [0,0,0,0].
    colors = {0: (0, 0, 0xFF, 0xFF)}  # opaque red
    pal = bytearray(make_palette(colors))
    pal[1 * 4 + 0] = 0
    pal[1 * 4 + 1] = 0
    pal[1 * 4 + 2] = 0
    pal[1 * 4 + 3] = 0  # index 1 -> transparent sentinel
    pal = bytes(pal)

    # Checkerboard of opaque-red (0) and transparent (1).
    frame = []
    for row in range(height):
        for col in range(width):
            frame.append(0 if (row + col) % 2 == 0 else 1)

    # Distinct per-pixel background so a wrong skip is detectable.
    bg = [(0x1000 + i) & 0xFFFF for i in range(width * height)]

    # Build the EAF bytes WITHOUT the encoder's used-index guard (this
    # case deliberately exercises a [0,0,0,0] used index to verify the
    # renderer-fidelity transparency model). We hand-encode via the
    # same encoder but with a guard-safe placeholder, then patch the
    # palette entry to all-zero -- byte layout is identical, only the
    # palette bytes change, so the round-trip remains valid.
    safe_pal = bytearray(pal)
    safe_pal[1 * 4 + 3] = 0xFF  # temporarily opaque so encode() accepts
    blob = bytearray(eaf_encode.encode([frame], width, height,
                                       bytes(safe_pal), block_height=height))
    # Patch index-1 alpha byte back to 0 in EVERY palette copy in the
    # file, then fix the container checksum (sum of bytes from off 16).
    needle = bytes(safe_pal)
    start = blob.find(needle)
    while start != -1:
        blob[start + 1 * 4 + 3] = 0x00
        start = blob.find(needle, start + 1)
    new_chk = sum(blob[16:]) & 0xFFFFFFFF
    blob[8:12] = struct.pack("<I", new_chk)

    try:
        _c, decoded = eaf_refdec.decode(bytes(blob), background=bg)
    except Exception as exc:  # noqa: BLE001
        FAILURES.append("%s: refdec raised %r" % (name, exc))
        traceback.print_exc()
        return

    want = render_expected(frame, width, height, pal, background=bg)
    got = decoded[0]["rgb565"]
    if check(got == want, "%s: transparency render mismatch got=%r want=%r"
             % (name, got, want)):
        # Also assert the structural intent: transparent slots kept bg,
        # opaque slots became red (0xF800).
        red = expected_rgb565(0, 0, 0xFF)
        struct_ok = True
        for i, idx in enumerate(frame):
            exp = red if idx == 0 else bg[i]
            if got[i] != exp:
                struct_ok = False
                FAILURES.append("%s: pixel %d idx=%d got 0x%04X want 0x%04X"
                                % (name, i, idx, got[i], exp))
                print("  FAIL: " + FAILURES[-1])
                break
        if struct_ok:
            print("  ok")


def opaque_black_case():
    """An all opaque-black [0,0,0,255] frame must round-trip to 0x0000
    everywhere (NOT treated as transparent: A=255 fails the all-zero
    sentinel test, gfx_eaf_dec.c:290-292)."""
    name = "opaque_black_frame"
    width, height = 12, 9
    pal = make_palette({0: (0, 0, 0, 0xFF)})  # index 0 = opaque black
    frame = [0] * (width * height)
    print("[case] %s (w=%d h=%d)" % (name, width, height))
    try:
        # Non-zero background proves opaque black actually WRITES 0x0000
        # (it must overwrite the background, unlike a transparent index).
        blob = eaf_encode.encode([frame], width, height, pal, block_height=4)
        _c, decoded = eaf_refdec.decode(blob, background=0x1234)
    except Exception as exc:  # noqa: BLE001
        FAILURES.append("%s: pipeline raised %r" % (name, exc))
        traceback.print_exc()
        return
    got = decoded[0]["rgb565"]
    if check(all(v == 0x0000 for v in got) and len(got) == width * height,
             "%s: opaque black did not produce all-0x0000 (sample=%r)"
             % (name, got[:8])):
        print("  ok")


def mostly_black_frame(width, height):
    """A mostly-opaque-black frame with a small bright disc -- the
    realistic status-circle shape: huge flat run-friendly background +
    a compact non-flat blob. Index 0 = opaque black background, index 1
    = white disc."""
    cx, cy = width / 2.0, height / 2.0
    r = min(width, height) * 0.18
    f = []
    for row in range(height):
        for col in range(width):
            dx = col - cx
            dy = row - cy
            f.append(1 if (dx * dx + dy * dy) <= r * r else 0)
    return f


RLE = None  # bound in main() after eaf_encode import-time constant exists


def main():
    global RLE
    RLE = eaf_encode.EAF_ENCODING_RLE
    RAW = eaf_encode.EAF_ENCODING_RAW
    print("=== EAF encoder round-trip ===")

    # --- Primary case: 3 frames, 16x16, gradient, block_height divides ---
    frames, w, h, pal = gradient_frames()
    run_case("primary_3f_16x16", frames, w, h, pal, block_height=8)

    # --- Edge: 1 frame ---
    run_case("single_frame", [frames[0]], w, h, pal, block_height=8)

    # --- Edge: non-multiple block_height (16 not divisible by 5) ---
    run_case("non_multiple_bh", frames, w, h, pal, block_height=5)

    # --- Edge: block_height == height (single block) ---
    run_case("single_block", frames, w, h, pal, block_height=h)

    # --- Edge: max palette (use a wide spread of all 256 indices) ---
    big_colors = {}
    for i in range(256):
        # spread B/G/R across the index; keep A=0xFF so no all-zero entry
        big_colors[i] = (i, (i * 7) & 0xFF, (i * 13) & 0xFF, 0xFF)
    big_pal = make_palette(big_colors)
    # 8x32 frame walking through every index 0..255 exactly once.
    bw, bh_img = 8, 32
    max_frame = [i for i in range(256)]
    run_case("max_palette_256", [max_frame], bw, bh_img, big_pal, block_height=7)

    # --- Edge: all-black frame (every pixel index 0 = opaque black) ---
    black_pal = make_palette({0: (0, 0, 0, 0xFF)})
    black_frame = [0] * (16 * 16)
    run_case("all_black", [black_frame], 16, 16, black_pal, block_height=4)

    # ===================================================================
    # RLE round-trip cases (encoding=EAF_ENCODING_RLE, tag 0x00). The
    # SAME pixel-exact + checksum + structural assertions as the RAW
    # cases above, plus per-block RLE structural validation in run_case.
    # ===================================================================

    # RLE primary: same gradient set, block_height divides height.
    run_case("rle_primary_3f_16x16", frames, w, h, pal, block_height=8,
             encoding=RLE)
    # RLE single frame.
    run_case("rle_single_frame", [frames[0]], w, h, pal, block_height=8,
             encoding=RLE)
    # RLE non-multiple block_height (16 % 5 != 0 -> padded final strip;
    # the padding rows are still RLE'd to a full width*block_height).
    run_case("rle_non_multiple_bh", frames, w, h, pal, block_height=5,
             encoding=RLE)
    # RLE single block.
    run_case("rle_single_block", frames, w, h, pal, block_height=h,
             encoding=RLE)
    # RLE max palette (256 distinct indices each appearing once -> worst
    # case: every run length 1, exercises the no-compression path while
    # still producing a structurally valid RLE stream).
    run_case("rle_max_palette_256", [max_frame], bw, bh_img, big_pal,
             block_height=7, encoding=RLE)
    # RLE pure-black runs: a single huge flat run per strip.
    run_case("rle_all_black", [black_frame], 16, 16, black_pal,
             block_height=4, encoding=RLE)
    # RLE full-width single-colour run, single block.
    run_case("rle_full_width_run", [[0] * (20 * 3)], 20, 3, black_pal,
             block_height=3, encoding=RLE)
    # RLE run > 255: a 600-pixel single-block flat strip forces the
    # 600-byte run to split into multiple (255,..)+(remainder,..) pairs.
    big_flat = [0] * 600
    run_case("rle_run_over_255", [big_flat], 600, 1, black_pal,
             block_height=1, encoding=RLE)
    # RLE run >> 255 over a 2-D strip (40x40 = 1600 of one colour ->
    # several 255-capped pairs in one block).
    run_case("rle_run_far_over_255", [[0] * (40 * 40)], 40, 40, black_pal,
             block_height=40, encoding=RLE)
    # RLE gradient single frame (alternating values -> many short runs;
    # confirms correctness when RLE does NOT compress well).
    run_case("rle_gradient", [frames[0]], w, h, pal, block_height=4,
             encoding=RLE)
    # RLE mostly-black status-circle-shaped frames, multi-block.
    mb = [mostly_black_frame(34, 34) for _ in range(3)]
    mb_pal = make_palette({0: (0, 0, 0, 0xFF), 1: (0xFF, 0xFF, 0xFF, 0xFF)})
    run_case("rle_mostly_black_34x34", mb, 34, 34, mb_pal, block_height=8,
             encoding=RLE)

    # --- SIZE SANITY: RLE must be materially smaller than RAW for a
    #     mostly-flat frame set (this is the whole reason RLE exists --
    #     status_*.eaf must fit the emote partition). ---
    size_w = size_h = 34
    size_frames = [mostly_black_frame(size_w, size_h) for _ in range(3)]
    size_pal = make_palette({0: (0, 0, 0, 0xFF), 1: (0xFF, 0xFF, 0xFF, 0xFF)})
    raw_blob = eaf_encode.encode(size_frames, size_w, size_h, size_pal,
                                 block_height=8, encoding=RAW)
    rle_blob = eaf_encode.encode(size_frames, size_w, size_h, size_pal,
                                 block_height=8, encoding=RLE)
    raw_sz, rle_sz = len(raw_blob), len(rle_blob)
    saved = raw_sz - rle_sz
    pct = (100.0 * saved / raw_sz) if raw_sz else 0.0
    print("[size] mostly-black 34x34x3: RAW=%d B  RLE=%d B  "
          "saved=%d B (%.1f%% smaller)" % (raw_sz, rle_sz, saved, pct))
    check(rle_sz < raw_sz,
          "size: RLE (%d) not smaller than RAW (%d) for a mostly-flat "
          "frame set" % (rle_sz, raw_sz))
    # Demand a real, material saving (mostly-flat must compress a lot).
    check(pct >= 50.0,
          "size: RLE only %.1f%% smaller than RAW (expected >=50%% for a "
          "mostly-flat frame)" % pct)
    # And confirm that smaller RLE blob still decodes pixel-exact.
    want_mb = render_expected(size_frames[0], size_w, size_h, size_pal)
    _c, dec_mb = eaf_refdec.decode(rle_blob)
    check(dec_mb[0]["rgb565"] == want_mb,
          "size: RLE-compressed mostly-black frame did not decode "
          "pixel-exact")

    # --- Golden RGB565 vectors anchored BY HAND to gfx_eaf_dec.c ---
    golden_case()

    # --- Opaque black [0,0,0,255] round-trips to 0x0000 (writes it) ---
    opaque_black_case()

    # --- encode() RAISES on a used [0,0,0,0] index; not on unused ---
    encode_rejects_transparent_used_index()

    # --- Transparency: [0,0,0,0] index skipped over a known bg ---
    transparency_case()

    print()
    if FAILURES:
        print("RESULT: FAIL (%d failure(s))" % len(FAILURES))
        for f in FAILURES:
            print("  - " + f)
        return 1
    print("RESULT: PASS (all cases)")
    return 0


if __name__ == "__main__":
    sys.exit(main())
