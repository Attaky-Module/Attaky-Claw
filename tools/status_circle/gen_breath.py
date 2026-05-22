"""Breathing status-circle frame generator (host tool).

Produces, per logical colour, a closed-loop sequence of 8-bit indexed
frames plus a 256-entry ``[B,G,R,A]`` palette directly consumable by
``tools.eaf.eaf_encode.encode(frames, canvas, canvas, palette)``.

This implements the breathing-disc asset generator for the
status-cluster redesign: a soft breathing colour circle, soft glow
edge, combined brightness + small scale pulse over a periodic loop,
drawn on the true-black emote background (design section 11.1: "RGB565,
no separate alpha -- the circle is drawn on the true-black emote
background so black reads as background").

SOLID-DISC REDESIGN (hardware-visual gate user decision):
On the real device the prior centre-dark glow RING ("centre darker <
edge < mid-bright") read as a circle with a HOLE punched in the middle
and was rejected on hardware. The required look is now a SOLID FILLED
glowing disc: the centre is the BRIGHTEST (or a near-uniform bright
plateau) and brightness is monotone NON-INCREASING outward to a soft
cosine glow edge that fades into the opaque-black background -- NO dark
core, NO interior local minimum, NO hole. The disc is also smaller
(now ~20 px after the hardware-visual iterations: the user
asked for a smaller circle and finer brightness to remove the choppy
breath stepping; the disc shrank through ~28 -> ~26 -> 20 px). This
SUPERSEDES the obsolete "centre darker, edge lighter"
intent and the prior F1 "centre < disc_edge < mid (strict)"
contract; both are now void by user hardware decision and the test
contract is inverted to the solid-disc model accordingly.

Pure Python 3 stdlib only (no PIL / numpy), mirroring the
dependency-free, decoder-cited style of the sibling tool
``tools/eaf/eaf_encode.py``.

Palette contract (FORMAT_NOTES.md section 4, gfx_eaf_dec.c:285-303):

  * Each entry is 4 bytes ``[B, G, R, A]``; on the panel it becomes
    ``rgb565 = ((R&0xF8)<<8) | ((G&0xFC)<<3) | ((B&0xF8)>>3)`` -- the
    low channel bits are discarded.
  * The all-zero entry ``[0,0,0,0]`` is the hardware TRANSPARENT
    sentinel (gfx_eaf_dec.c:290-292 -> gfx_anim.c:470-499 SKIPS those
    pixels). We never map a drawn pixel to it. Background black is the
    OPAQUE black ``[0,0,0,255]`` so it reliably paints over the emote
    face as a real black disc-of-nothing rather than punching a hole.

Palette layout chosen here (per colour, 256 entries):

  * index 0          -> opaque black ``[0,0,0,255]`` = background.
  * index 1 .. 255   -> a 255-step linear ramp from a dim version of
    the base colour up to the full base colour. The combined final
    pixel brightness is first quantised to ``_BRIGHTNESS_LEVELS``
    evenly-spaced discrete levels (RLE-compressibility, see that
    constant), then the quantised brightness maps to ramp index
    ``1 + round(b * 254)``; brightness 0 maps to index 0 (background).
    So only ``_BRIGHTNESS_LEVELS`` distinct ramp indices (+ background)
    are ever referenced -> equal-brightness regions are contiguous
    concentric bands -> long RLE runs. This keeps every used index off
    the ``[0,0,0,0]`` sentinel and gives a banded-but-perceptually-
    smooth glow with one palette per colour (a single shared palette
    cannot hold four independent colour ramps within 256 entries, so
    per-colour is the clean choice; the test reflects this).

All magic constants are documented inline with the design/plan section
that motivates them.
"""

import math

# Base colours, design section 2 (NORMAL=blue, RECORDING=green, SPEAKING=
# yellow, NOTIFICATION=red). Values are 8-bit (R, G, B). Channels are
# pushed toward the panel's RGB565 ceiling so the breathing circle
# reads as a saturated status light after the decoder's 5/6/5-bit
# truncation (gfx_eaf_dec.c:294-300). Yellow is R+G high, B zero.
COLOURS = {
    "blue":   (0x14, 0x6E, 0xFF),
    "green":  (0x1E, 0xD7, 0x4A),
    "yellow": (0xFF, 0xD0, 0x14),
    "red":    (0xFF, 0x32, 0x2D),
}

# ---- breathing / gradient tuning constants -----------------------
#
# All are documented against design section 3 ("soft glow edge,
# combined brightness + scale breathing cycle") and the
# hardware-visual gate user decision (solid filled glowing
# disc, no dark core, now ~20px; supersedes prior centre-dark ring).

# Brightness floor/ceiling of the breath envelope. The mean luminance
# of the loop swings between BREATH_MIN and BREATH_MAX (a single
# cosine, one rise + one fall per loop -> "breathing", design section 3).
#
# hardware-visual: smaller disc (20/canvas32) + finer
# brightness (N=32) to remove temporal banding (the choppy stepping);
# smaller canvas pays the size for more levels; scale 3px gentler
# integer step. Smoother (36f) + pronounced bright/dim
# (BREATH_MIN~0.25); RLE encoder makes
# the 32px canvas cheap. The prior 0.55..1.00 swing read as too subtle
# on real hardware ("the bright/dim swing is not pronounced enough" --
# the breath was barely perceptible). The dim
# floor is dropped to ~0.25 so the dim phase is clearly dim and the
# bright phase clearly bright: a strong, obvious pulse. It stays a
# SINGLE raised cosine (one rise + one fall, one max, one min); the
# wider range only deepens the existing single hump, it does NOT add a
# second hump. The floor stays > 0 so the disc never fully blinks out
# (a breath, not a flash).
_BREATH_MIN = 0.25   # dimmest point of the breath (clearly dim, not off)
_BREATH_MAX = 1.00   # brightest point of the breath

# Radial intensity profile inside the visual disc -- SOLID-DISC model
# (hardware-visual gate user decision: solid filled glowing
# disc, no dark core, now ~20px; supersedes prior centre-dark ring).
#
# Required shape (r_norm in [0,1]; 0 = centre, 1 = solid-disc edge):
#
#   * brightness(0) is the GLOBAL MAXIMUM (== 1.0 before the breath
#     envelope / glow scaling).
#   * brightness(r) is monotone NON-INCREASING as r grows. There is no
#     interior local minimum -> no dark core, no hole.
#   * a bright near-uniform PLATEAU covers the inner core out to
#     ``_CORE_FRAC`` of the radius (so the centre reads as a solid
#     bright body, not a single hot pixel), then a smooth cosine
#     shoulder eases the body brightness down to ``_RIM_LEVEL`` at the
#     solid-disc edge (r_norm == 1). _RIM_LEVEL stays well above 0 so
#     the body is clearly solid right up to where the soft glow halo
#     takes over.
#
# Net ordering (before breath/glow): centre == plateau == global max
# >= any mid-radius sample >= disc-edge sample, all monotone
# non-increasing, no interior minimum. This is the exact inversion the
# user's on-hardware decision requires.
_CORE_FRAC = 0.30    # inner fraction of the radius held at the flat
                     # bright plateau (== global max). 0.30 of a ~10px
                     # radius (20px visual disc) is a ~3px solid bright
                     # core (no single-pixel hotspot, no dark centre)
                     # while still leaving a mid-radius sample strictly
                     # below the centre max (centre > mid, the solid-
                     # disc "no hole" contract).
_RIM_LEVEL = 0.62    # relative brightness at the solid-disc edge
                     # (r_norm == 1), before the soft glow halo. Kept
                     # high so the disc body stays visibly solid (a
                     # filled disc, not a ring) until the glow falloff.

# Soft glow: beyond the visual edge the colour fades to the black
# background over a halo of _GLOW_PX pixels with a smooth (cosine)
# falloff -> monotone non-increasing luminance from edge to corner
# (design section 3 "soft glow edge"). The halo must stay inside the
# canvas corner so the corner is pure background. A small halo keeps
# the .eaf tight (hardware decision: keep it small).
_GLOW_PX = 3.0

# Brightness quantisation -> RLE-compressibility (size gate).
# The encoder default is EAF_ENCODING_RLE, which encodes
# each scanline as (count,index) pairs. A 255-step smooth radial+breath
# gradient gives almost every horizontal pixel a DIFFERENT palette
# index -> run length ~1 -> the RLE pair stream is LARGER than RAW
# (~105KB/asset; 4 assets overflow the emote partition). The shipping
# assets are RLE-small because they have long equal-index runs.
#
# Fix: quantise the COMBINED final per-pixel brightness (radial profile
# * breath envelope * coverage) into a MODEST number of evenly-spaced
# discrete levels BEFORE mapping a level to a palette ramp index. Equal
# brightness then maps to an identical index, so equal-brightness
# regions become contiguous concentric bands -> each scanline crossing
# the disc is a few long runs -> RLE compresses dramatically (target
# well under the old ~53KB RAW; measured ~15-30KB).
#
# N = 32: hardware-visual: smaller disc (20/canvas32) +
# finer brightness (N=32) to remove temporal banding (the choppy
# stepping); smaller canvas pays the size for more levels; scale 3px
# gentler integer step. The breath is a GLOBAL brightness (+scale)
# pulse: with too few levels the whole disc only ever takes N discrete
# brightness values across the cycle, so as the breath envelope sweeps
# the brightness JUMPS between the N quantised steps -> visible
# temporal stepping (the choppy "ka" the user reported), independent
# of frame count. The prior N=16 made the breath step in only 16
# discrete brightnesses over the loop -> choppy. Doubling to N=32
# halves the step size for a noticeably smoother breath; the shrunk
# disc (20px visual / 32px canvas) keeps the per-frame RLE index data
# small enough that the extra levels are size-neutral-ish. Across the
# 0.25..1.0 breath envelope 32 levels is ample dynamic range and at the
# ~20px visible disc of a small soft-glow ambient indicator it is below
# human spatial banding perception, so it still reads as smooth. The
# quantisation is applied to the brightness SCALAR (not the palette) so
# it is deterministic and preserves centre==global-max, monotone non-
# increasing, no interior minimum, soft-glow monotone to corner,
# corner==opaque-black across all frames. Tunable: raise it if a future
# amplitude change needs more range (do NOT lower the breath amplitude
# params to compensate); lower it for even smaller .eaf at the cost of
# coarser banding (and a choppier breath).
_BRIGHTNESS_LEVELS = 32


def _clamp01(x):
    if x < 0.0:
        return 0.0
    if x > 1.0:
        return 1.0
    return x


def _radial_brightness(r_norm):
    """Intensity multiplier in [0,1] at normalised radius ``r_norm``
    (0 = centre, 1 = solid-disc edge).

    SOLID-DISC profile (hardware-visual gate user decision): a flat
    bright plateau (== global max 1.0) over the
    inner ``_CORE_FRAC`` of the radius, then a smooth cosine shoulder
    easing down to ``_RIM_LEVEL`` at the disc edge. The result is
    monotone NON-INCREASING in ``r_norm`` with its single maximum at
    the centre and NO interior local minimum -> a solid filled glowing
    disc, no dark core, no hole. This replaces the obsolete blended
    sin-ring profile that produced the rejected centre-dark hole."""
    r = _clamp01(r_norm)
    if r <= _CORE_FRAC:
        # Flat bright core plateau == global max.
        return 1.0
    # Cosine shoulder from 1.0 at r==_CORE_FRAC down to _RIM_LEVEL at
    # r==1.0. ``t`` in [0,1]; 0.5*(1+cos(pi*t)) goes 1 -> 0 smoothly
    # and is strictly monotone non-increasing on [0,1], so the whole
    # profile stays monotone non-increasing with no interior minimum.
    t = (r - _CORE_FRAC) / (1.0 - _CORE_FRAC)
    shoulder = 0.5 * (1.0 + math.cos(math.pi * t))  # 1 -> 0
    return _RIM_LEVEL + (1.0 - _RIM_LEVEL) * shoulder


def _coverage(dist, radius):
    """Disc coverage in [0,1] at pixel distance ``dist`` from centre
    for a circle of ``radius``:

      * inside the disc (dist <= radius)      -> 1.0 (solid body)
      * glow halo (radius .. radius+_GLOW_PX) -> smooth cosine fade
      * beyond the halo                       -> 0.0 (background)

    Monotone non-increasing in ``dist`` -> satisfies the "soft glow,
    monotone to corner" contract (design section 3). The only edge
    softness is the _GLOW_PX cosine falloff below; the disc body
    itself is a hard solid (no separate anti-alias ramp)."""
    if dist <= radius:
        return 1.0
    if dist <= radius + _GLOW_PX:
        # Cosine glow: 1 at the edge, 0 at radius+_GLOW_PX.
        t = (dist - radius) / _GLOW_PX
        return 0.5 * (1.0 + math.cos(math.pi * t))
    return 0.0


def _breath_envelope(frame_index, frames):
    """Mean-brightness multiplier for ``frame_index`` of an
    ``frames``-long CLOSED loop. A single raised cosine over the full
    period: exactly one minimum and one maximum, value at frame
    ``frames`` equals value at frame 0 (design section 3 "breathing cycle";
    plan Task 2.1 "single rise + single fall, closed loop")."""
    # phase in [0, 2*pi); cos gives max at phase 0, min at phase pi.
    phase = 2.0 * math.pi * (frame_index / frames)
    # cos(phase): 1 -> -1 -> 1 over the loop. Map to [_BREATH_MIN,
    # _BREATH_MAX].
    unit = 0.5 * (1.0 + math.cos(phase))  # 1 at f=0, 0 at f=N/2
    return _BREATH_MIN + (_BREATH_MAX - _BREATH_MIN) * unit


def _build_palette(base_rgb):
    """256-entry [B,G,R,A] palette for one colour.

    index 0   = opaque black background ``[0,0,0,255]``.
    index k>0 = base colour scaled by ``k/255`` (a 255-step ramp),
    stored as ``[B,G,R,255]``. No entry is the all-zero transparent
    sentinel for any index a frame can reference (gfx_eaf_dec.c:
    290-292; FORMAT_NOTES.md section 4)."""
    r0, g0, b0 = base_rgb
    pal = bytearray(256 * 4)
    # index 0: opaque black background.
    pal[0] = 0      # B
    pal[1] = 0      # G
    pal[2] = 0      # R
    pal[3] = 255    # A (opaque -> NOT the [0,0,0,0] transparent entry)
    for k in range(1, 256):
        scale = k / 255.0
        b = int(round(b0 * scale))
        g = int(round(g0 * scale))
        r = int(round(r0 * scale))
        # Guard: a non-background ramp index must never collapse to the
        # all-zero sentinel. Force A=255 (opaque) and, if the scaled
        # colour rounded to pure black, nudge the lowest channel so the
        # entry is a defined (near-black) colour rather than the
        # transparent [0,0,0,0] (gfx_eaf_dec.c:290-292).
        if b == 0 and g == 0 and r == 0:
            # pick the channel with the largest base weight to nudge.
            mx = max(r0, g0, b0)
            if mx == r0:
                r = 1
            elif mx == g0:
                g = 1
            else:
                b = 1
        o = k * 4
        # min(255, ...) is belt-and-suspenders, not a reachable
        # overflow path: base channels are <= 0xFF and scale <= 1.0,
        # so b/g/r already lie in 0..255. Kept as a cheap invariant
        # guard against future tuning that scales above 1.0.
        pal[o + 0] = min(255, b)
        pal[o + 1] = min(255, g)
        pal[o + 2] = min(255, r)
        pal[o + 3] = 255
    return bytes(pal)


def generate(colour, frames=36, canvas=32, visual_diameter=20,
             scale_breath_px=3.0):
    """Generate the breathing circle for ``colour``.

    Args:
      colour: one of ``COLOURS`` ("blue"/"green"/"yellow"/"red").
      frames: number of frames in the closed loop. Default 36
        (hardware-visual gate: smoother (36f) -- the prior
        24-frame loop still read as not smooth enough on real hardware
        ("breathing not smooth enough, need more frames"); 36 frames
        give a 1.5x finer breath gradient again over the loop. The RLE
        encoder default makes the extra frames almost free in .eaf
        size. Still a single rise+fall closed loop, just finer-grained;
        design section 11.1. 36 stays divisible by 4 so the half-cycle
        exact-tie determinism path is still exercised.
      canvas: square canvas size in px. Default 32 (hardware-visual:
        smaller disc (20/canvas32) + finer brightness
        (N=32) to remove temporal banding (choppiness); smaller canvas pays the
        size for more levels. 32 fits ``visual_diameter`` (20) +
        2*``_GLOW_PX`` (6) + 2*the scale-breath amplitude (~6) -> 32px
        with the corner-fit margin held by the guard below. Shrinking
        the canvas 40 -> 32 cuts the per-frame RLE index data, which
        budgets the extra brightness levels at roughly size-neutral.
        The grown disc + glow must never clip and the corners must stay
        opaque-black background across ALL frames (corner-fit
        invariant). The RLE encoder default makes the canvas cheap (a
        mostly-background canvas RLE-compresses far smaller than the old
        RAW blob). An odd canvas also works (no exact integer centre)
        and is exercised by the test. The canvas must stay >=
        visual_diameter + 2*_GLOW_PX + scale headroom or the call is
        rejected (corner-fit guard below).
      visual_diameter: nominal solid-disc diameter in px. Default 20
        (hardware-visual: the user asked for a smaller
        circle; 26 -> 20 shrinks the visible disc and, as a side
        benefit, fewer disc pixels -> smaller RLE index data -> budget
        for the finer N=32 brightness quantisation that removes the
        temporal banding (the choppy stepping); supersedes the prior
        26/28px discs and the even older ~36px centre-dark ring). The
        glow halo extends _GLOW_PX beyond this and the disc
        additionally grows by the
        scale-breath amplitude at the breath peak, but the grown disc +
        glow must still stay inside the canvas corner.
      scale_breath_px: peak-to-trough variation of the disc's bounding
        radius across the loop, in px (the "scale" half of the
        combined brightness+scale breath, design section 3). Default
        3.0 (hardware-visual: scale 3px gentler integer
        step -- on the smaller 20px disc an integer-pixel scale step of
        4 snaps visibly (a ~20% radius jump per step looks choppy);
        3px is still a clearly visible ~15% radius pulse but with a
        gentler integer step, so the scale half of the breath reads as
        smoother. Brightness and scale stay phase-coupled: brightest
        frame == largest disc). Must be ``>= 1.0`` and small. The
        breath is realised on an integer
        pixel lattice (see ``scale_amp`` below), so for
        ``scale_breath_px >= 1.0`` the realised bounding-radius spread
        is ``> 0`` and ``<= scale_breath_px``. A sub-1px value in the
        open interval ``(0, 1)`` is REJECTED with ``ValueError``: the
        integer-pixel breath scheme cannot realise a sub-1px pulse
        (it would always round up to a full 1px breath, so honouring
        the documented ``<= scale_breath_px`` bound is impossible).
        This rejection-rather-than-silent-violation habit mirrors
        ``tools/eaf/eaf_encode.py``.

    Returns:
      ``(frames_list, palette)`` where ``frames_list`` is a list of
      ``frames`` flat row-major sequences of ``canvas*canvas`` ints in
      0..255, and ``palette`` is exactly 1024 ``[B,G,R,A]`` bytes.
      Directly feed to ``eaf_encode.encode(frames_list, canvas,
      canvas, palette)``.
    """
    if colour not in COLOURS:
        raise ValueError("unknown colour %r; expected one of %s"
                          % (colour, sorted(COLOURS)))
    if frames < 2:
        raise ValueError("need at least 2 frames for a loop")
    if canvas < 4:
        raise ValueError("canvas too small")
    if scale_breath_px <= 0:
        raise ValueError("scale_breath_px must be > 0 (the pulse must "
                         "be real, plan Task 2.1)")
    if 0 < scale_breath_px < 1.0:
        # The breath is quantised to whole pixels (scale_amp =
        # max(1, floor(scale_breath_px))), so any 0 < x < 1 would
        # silently become a full 1px pulse and violate the documented
        # "realised spread <= scale_breath_px" contract. Reject rather
        # than lie (same habit as tools/eaf/eaf_encode.py).
        raise ValueError(
            "scale_breath_px must be >= 1.0; the integer-pixel breath "
            "scheme cannot realise a sub-1px pulse")

    base_rgb = COLOURS[colour]
    palette = _build_palette(base_rgb)

    # cx == cy here (square canvas). The split into two names is
    # deliberate: it keeps the per-axis (dx/dy) loop maths explicit and
    # leaves the door open to a future non-square canvas without a
    # rename churn.
    cx = (canvas - 1) / 2.0
    cy = (canvas - 1) / 2.0
    base_radius = visual_diameter / 2.0

    # The disc radius breathes in phase with the brightness envelope
    # so brightness and scale pulse together (design section 3 "combined
    # brightness + scale"). The exact integer-pixel scheme is set up
    # below (``scale_amp``); here we only need its worst-case extent
    # for the canvas-fit guard.
    scale_amp_max = max(1, int(math.floor(scale_breath_px)))

    # Keep the largest possible (disc + glow) inside the canvas corner
    # so the corner stays pure background. If the user asked for a disc
    # that would overflow, the call is rejected (shrink the disc).
    max_reach = math.hypot(cx, cy)  # centre -> corner distance
    safety = base_radius + scale_amp_max + _GLOW_PX
    if safety >= max_reach:
        raise ValueError(
            "visual_diameter + glow + scale-breath (%.1fpx) does not fit "
            "in a %dpx canvas (corner reach %.1fpx); shrink "
            "visual_diameter" % (safety, canvas, max_reach))

    # Scale-breath as an INTEGER, one-sided pixel growth.
    #
    # The contract (plan Task 2.1) measures the breath via the max
    # ``hypot`` of any non-background pixel and requires its
    # peak-to-trough spread to be > 0 and <= ``scale_breath_px``.
    # A sub-pixel sinusoidal radius does NOT satisfy this on an integer
    # lattice: the realised outermost pixel jitters by up to ~sqrt(2)px
    # regardless of how small the continuous swing is, so a 1.5px
    # continuous swing measured 1.58px (lattice overshoot).
    #
    # Fix: the disc's hard outer boundary breathes by a whole number of
    # pixels ``scale_amp`` = max(1, floor(scale_breath_px)) on a
    # one-sided range [base_radius, base_radius + scale_amp], and every
    # non-bg pixel is hard-clipped to that integer boundary (squared-
    # distance compare, no sub-pixel glow tail beyond it). The realised
    # max non-bg radius is then a deterministic function of an integer
    # boundary; its spread is bounded by, and stays at or under,
    # ``scale_breath_px`` while remaining > 0 (the pulse is real).
    scale_amp = scale_amp_max

    frames_list = []
    for fi in range(frames):
        env = _breath_envelope(fi, frames)            # brightness mult
        # Radius breathes in phase with brightness (brightest frame is
        # also the largest disc): a single raised cosine over the loop,
        # quantised to integer pixels, one-sided in [0, scale_amp].
        #
        # DETERMINISM (F2): ``grow_px`` must be byte-identical across
        # libm/platform builds so the committed ``.eaf`` assets (and
        # the 3MB size gate that depends on them) are reproducible.
        #
        # The naive ``int(round(scale_amp * 0.5*(1+cos(phase))))`` is
        # NOT host-stable: for a frame count divisible by 4 the
        # half-cycle frames (fi == frames/4, 3*frames/4) have
        # phase == pi/2 or 3*pi/2, where the EXACT value is
        # ``scale_amp * 0.5`` -- a perfect rounding tie. libm ``cos``
        # at pi/2 returns ~6.1e-17 (not exactly 0) with a sign/last-bit
        # that varies by platform, and Python's banker's ``round`` on
        # ``N + 0.5 +/- 1ulp`` then flips between ``N`` and ``N+1``
        # depending on the host. That is the non-determinism the review
        # flagged.
        #
        # Deterministic fix, two parts:
        #  (1) Detect the exact tie analytically in INTEGER frame-index
        #      arithmetic instead of trusting ``cos`` precision there:
        #      cos(2*pi*fi/frames) == 0 exactly iff ``4*fi`` is an odd
        #      multiple of ``frames`` (i.e. (4*fi) % frames == 0 and
        #      ((4*fi)//frames) is odd). At those frames grow_unit is
        #      EXACTLY 0.5 by construction, independent of libm.
        #  (2) Quantise with an explicit, documented "round half up"
        #      tie rule via ``math.floor(value + 0.5)`` applied to a
        #      value first snapped to a fixed grid with a small epsilon
        #      so a libm result that is ``T - 1ulp`` (just under the
        #      tie) is still treated as the tie. floor(x+0.5) is itself
        #      platform-stable for non-tie inputs; the epsilon-snap +
        #      exact-tie detection make the tie deterministic too.
        phase = 2.0 * math.pi * (fi / frames)
        four_fi = 4 * fi
        if four_fi % frames == 0 and ((four_fi // frames) % 2 == 1):
            # Exact analytic half-cycle tie: grow_unit == 0.5 exactly,
            # no libm involved. Documented tie rule: round half UP.
            grow_unit = 0.5
        else:
            grow_unit = 0.5 * (1.0 + math.cos(phase))  # 1 at f=0, 0 mid
        # Snap to a fixed 1e-9 grid so a libm value that is the tie
        # minus a few ulp still lands on the tie, then round-half-up
        # deterministically (math.floor(x + 0.5), not banker's round).
        scaled = scale_amp * grow_unit
        snapped = math.floor(scaled / 1e-9 + 0.5) * 1e-9
        grow_px = int(math.floor(snapped + 0.5))      # 0..scale_amp
        if grow_px < 0:
            grow_px = 0
        elif grow_px > scale_amp:
            grow_px = scale_amp
        radius = base_radius + grow_px
        # Hard integer outer clip for the non-bg region (squared form
        # to keep the boundary on a clean lattice ring).
        outer = radius + _GLOW_PX
        outer_sq = outer * outer

        buf = bytearray(canvas * canvas)  # default 0 == background
        for y in range(canvas):
            dy = y - cy
            row = y * canvas
            for x in range(canvas):
                dx = x - cx
                if dx * dx + dy * dy > outer_sq:
                    continue  # hard background clip (bounds the breath)
                dist = math.hypot(dx, dy)
                cov = _coverage(dist, radius)
                if cov <= 0.0:
                    continue  # stays index 0 (opaque-black background)
                r_norm = dist / radius if radius > 0 else 0.0
                radial = _radial_brightness(r_norm)
                bright = _clamp01(env * radial * cov)
                # RLE-compressibility quantisation (size gate,
                # see _BRIGHTNESS_LEVELS): snap the COMBINED
                # final brightness scalar to one of _BRIGHTNESS_LEVELS
                # evenly-spaced levels in (0, 1] BEFORE mapping to a
                # ramp index. ``bright`` here is already > 0 (cov <= 0
                # pixels were skipped above and stay index 0), so we
                # quantise the open-top range: level = ceil(bright * N)
                # in 1..N, then snap bright back to level/N. ceil keeps
                # the centre (bright == 1.0 -> level N -> the brightest
                # ramp index, the global max) and is monotone non-
                # decreasing in ``bright``, so the existing monotone-
                # non-increasing-in-radius profile stays monotone (it
                # just steps in N discrete concentric bands). Equal
                # brightness -> identical level -> identical index ->
                # contiguous bands -> long RLE runs. Deterministic
                # (pure integer ceil on a snapped value, no libm).
                lvl = int(math.ceil(bright * _BRIGHTNESS_LEVELS - 1e-9))
                if lvl < 1:
                    lvl = 1
                elif lvl > _BRIGHTNESS_LEVELS:
                    lvl = _BRIGHTNESS_LEVELS
                bright = lvl / float(_BRIGHTNESS_LEVELS)
                # Map [0,1] brightness onto ramp indices 1..255 (index
                # 0 is reserved for the opaque-black background).
                #
                # The OUTER extent of the non-background region must be
                # governed purely by the geometric ``radius`` (+ the
                # fixed glow halo), NOT by the brightness envelope:
                # otherwise the brightest frame's glow tail would round
                # one extra ring of borderline pixels above index 1 and
                # the non-bg bounding radius would swing by more than
                # ``scale_breath_px`` (plan Task 2.1: bounding-radius
                # spread <= configured scale-breath amplitude). So any
                # covered pixel (cov > 0) gets at least index 1; its
                # outer boundary is then exactly ``radius + _GLOW_PX``,
                # whose peak-to-trough across the loop is precisely
                # ``scale_breath_px`` (radius breathes by that amount,
                # _GLOW_PX is constant).
                idx = 1 + int(round(bright * 254.0))
                if idx < 1:
                    idx = 1
                elif idx > 255:
                    idx = 255
                buf[row + x] = idx
        frames_list.append(list(buf))

    return frames_list, palette
