"""Visual-spec contract test for the breathing status-circle generator.

This is the TDD oracle for the status-cluster redesign breathing-disc
generator, updated for the SOLID-DISC redesign (hardware-visual gate
user decision: solid filled glowing disc, no dark core, ~28-30px;
supersedes the prior centre-dark ring). The obsolete
"centre < disc_edge < mid" / dim-core F1 assertions are intentionally
INVERTED here -- on the real device the old centre-dark profile read
as a circle with a hole punched in it and was rejected; the contract
is now "centre is the global max, monotone non-increasing outward, no
interior minimum / no hole".

What is asserted (each item maps 1:1 to the updated Task 2.1
contract):

  * shape / consumability  -- N frames of canvas*canvas ints 0..255, a
    1024-byte [B,G,R,A] palette, and a successful real
    ``tools.eaf.eaf_encode.encode`` (this also exercises the encoder's
    own [0,0,0,0]-transparent-sentinel guard, gfx_eaf_dec.c:290-292).
  * solid-disc radial      -- brightest frame: centre luma is the
    GLOBAL MAX and strictly greater than a mid-radius pixel;
    mid >= a disc-edge pixel; brightness is monotone NON-INCREASING
    centre->edge along >= 2 axes (+x and +y); NO interior local
    minimum / no dark hole (hardware-visual gate user decision:
    solid filled glowing disc, no dark core).
  * soft glow to bg        -- luminance is monotone non-increasing from
    the disc edge outward to the canvas corner; the corner is the
    opaque-black background (design section 3 "soft glow edge",
    section 11.1 "circle drawn on true-black emote background").
  * opaque-black bg        -- corner palette entry == [0,0,0,255], never
    [0,0,0,0] (FORMAT_NOTES.md section 4 transparency sentinel rule).
  * periodic breathing     -- per-frame mean luminance is a single
    smooth rise+fall (exactly one max run, one min run), non-constant,
    and a closed loop (frame[N] would equal frame[0]) (design section 3
    "combined brightness + scale breathing cycle"; plan Task 2.1).
  * pronounced (not subtle) -- the breath brightness pulse and the
    scale pulse each exceed a meaningful threshold so a regression
    back to the too-subtle pre-redesign swing fails the suite
    (hardware-visual: smaller disc (20/canvas32) + finer
    brightness (N=32) to remove temporal banding (choppiness); smaller canvas
    pays the size for more levels; scale 3px gentler integer step).
  * temporally smooth (not choppy) -- the number of DISTINCT
    centre-pixel luma values exercised over the closed loop must
    clear a fixed floor so a future over-coarsening regression (too
    few brightness levels -> the global breath pulse steps in visibly
    few quantised jumps -> the choppy stepping the user reported)
    fails the suite. This was RED at the old _BRIGHTNESS_LEVELS=16 and
    is GREEN at 32.
  * scale-breath bounded    -- max non-bg radius varies across frames by
    > 0 and <= the configured scale_breath_px amplitude.
  * four colours           -- blue/green/yellow/red each yield the
    expected dominant channel(s) at the brightest mid-radius pixel
    (design section 2 state->colour table).

Luminance is computed from the palette exactly as the device would see
it: index -> [B,G,R,A] -> RGB565-truncated channels (the decoder
discards the low bits via ``&0xF8`` / ``&0xFC``,
gfx_eaf_dec.c:294-300) -> Rec.601 luma. Pure stdlib unittest.
"""

import math
import os
import sys
import unittest

# Make the worktree root importable so ``tools.eaf.eaf_encode`` resolves
# regardless of where the test is launched from.
_THIS_DIR = os.path.dirname(os.path.abspath(__file__))
_ROOT = os.path.abspath(os.path.join(_THIS_DIR, os.pardir, os.pardir))
if _ROOT not in sys.path:
    sys.path.insert(0, _ROOT)

from tools.eaf import eaf_encode  # noqa: E402
from tools.status_circle import gen_breath  # noqa: E402


def _rgb565_truncated_channels(entry):
    """[B,G,R,A] palette entry -> (R8', G8', B8') after the decoder's
    RGB565 bit-truncation (gfx_eaf_dec.c:294-300: R/B keep 5 bits,
    G keeps 6 bits). This is what is actually shown on the panel."""
    b, g, r = entry[0], entry[1], entry[2]
    r5 = r & 0xF8
    g6 = g & 0xFC
    b5 = b & 0xF8
    return r5, g6, b5


def _luma_of_index(palette, idx):
    """Rec.601 luminance of palette index ``idx`` as the panel renders
    it (post RGB565 truncation). Monotone in perceived brightness."""
    entry = palette[idx * 4: idx * 4 + 4]
    r, g, b = _rgb565_truncated_channels(entry)
    return 0.299 * r + 0.587 * g + 0.114 * b


def _runs(values, key):
    """Count contiguous runs where ``key(prev, cur)`` holds, used to
    assert 'exactly one rise and one fall' across the breath cycle."""
    count = 0
    prev_state = None
    for i in range(1, len(values)):
        state = key(values[i - 1], values[i])
        if state and prev_state is not True:
            count += 1
        prev_state = state
    return count


# Defaults pin the solid-disc redesign sizing (hardware-visual:
# smaller disc (20/canvas32) + finer brightness (N=32) to
# remove temporal banding (choppiness); smaller canvas pays the size for more
# levels; scale 3px gentler integer step). Small visible disc (~20px)
# on a 32px canvas sized to fit the breath-peak grown disc + glow with
# margin so the corners stay opaque-black background across ALL frames.
CANVAS = 32
# gen_breath default frame count (closed loop). 36 frames give fine
# breath steps; with the engine fps 12 a 36/12 = 3.0s cycle. These
# assertions track the generator default (kept frame-count-agnostic
# where possible). 36 is divisible by 4 so the half-cycle exact-tie
# determinism path is still exercised.
FRAMES = 36
VISUAL_DIAMETER = 20
SCALE_BREATH_PX = 3.0


class GenBreathContract(unittest.TestCase):

    # ---- shape / consumability -----------------------------------

    def test_shape_and_eaf_encodable(self):
        for name in ("blue", "green", "yellow", "red"):
            frames, palette = gen_breath.generate(
                name, frames=FRAMES, canvas=CANVAS,
                visual_diameter=VISUAL_DIAMETER,
                scale_breath_px=SCALE_BREATH_PX)
            self.assertEqual(len(frames), FRAMES)
            for fr in frames:
                self.assertEqual(len(fr), CANVAS * CANVAS)
                for v in fr:
                    self.assertIsInstance(v, int)
                    self.assertGreaterEqual(v, 0)
                    self.assertLessEqual(v, 255)
            self.assertEqual(len(palette), 256 * 4)
            blob = eaf_encode.encode(frames, CANVAS, CANVAS, palette)
            self.assertIsInstance(blob, (bytes, bytearray))
            self.assertGreater(len(blob), 0)

    # ---- solid-disc radial profile -------------------------------

    def _brightest_frame(self, frames, palette):
        means = []
        for fr in frames:
            means.append(sum(_luma_of_index(palette, i) for i in fr) / len(fr))
        bi = means.index(max(means))
        return bi, frames[bi]

    def _effective_radius(self, frame_index, frames=FRAMES,
                          visual_diameter=VISUAL_DIAMETER,
                          scale_breath_px=SCALE_BREATH_PX):
        """Recompute the generator's actual breathed solid-disc radius
        for ``frame_index`` (F3: tests must sample the per-frame
        effective radius ``base_radius + grow_px``, NOT the static
        nominal ``visual_diameter/2``). This mirrors gen_breath's
        deterministic integer grow scheme exactly so a regression in
        either is caught."""
        base_radius = visual_diameter / 2.0
        scale_amp = max(1, int(math.floor(scale_breath_px)))
        four_fi = 4 * frame_index
        if four_fi % frames == 0 and ((four_fi // frames) % 2 == 1):
            grow_unit = 0.5
        else:
            grow_unit = 0.5 * (1.0 + math.cos(
                2.0 * math.pi * (frame_index / frames)))
        scaled = scale_amp * grow_unit
        snapped = math.floor(scaled / 1e-9 + 0.5) * 1e-9
        grow_px = int(math.floor(snapped + 0.5))
        grow_px = max(0, min(scale_amp, grow_px))
        return base_radius + grow_px

    def test_solid_disc_radial_profile(self):
        """Centre is the GLOBAL MAX, strictly brighter than mid;
        mid >= disc-edge; monotone non-increasing centre->edge on >= 2
        axes; NO interior local minimum / no dark hole. This is the
        exact inversion of the rejected centre-dark ring (hardware-
        visual gate user decision)."""
        frames, palette = gen_breath.generate(
            "blue", frames=FRAMES, canvas=CANVAS,
            visual_diameter=VISUAL_DIAMETER,
            scale_breath_px=SCALE_BREATH_PX)
        bi, fr = self._brightest_frame(frames, palette)
        cx = cy = (CANVAS - 1) / 2.0
        # F3: sample at the ACTUAL breathed solid-disc radius of the
        # brightest frame, not the static nominal radius.
        r_eff = self._effective_radius(bi)

        def lum_at(px, py):
            return _luma_of_index(
                palette, fr[int(round(py)) * CANVAS + int(round(px))])

        centre = lum_at(cx, cy)
        mid_x = lum_at(cx + r_eff * 0.5, cy)
        mid_y = lum_at(cx, cy + r_eff * 0.5)
        # Disc-edge sampled JUST inside the solid-disc boundary (one
        # pixel in so rounding cannot land in the soft-glow halo).
        disc_edge_x = lum_at(cx + (r_eff - 1.0), cy)
        disc_edge_y = lum_at(cx, cy + (r_eff - 1.0))

        # Centre is the GLOBAL maximum over the whole brightest frame
        # (no pixel anywhere is brighter than the centre -> the centre
        # is the brightest point, a solid bright core, never a hole).
        global_max = max(_luma_of_index(palette, i) for i in fr)
        self.assertEqual(centre, global_max,
                         "centre must be the global brightest pixel "
                         "(solid filled disc, no dark core)")
        # Centre STRICTLY brighter than a mid-radius pixel on both
        # sampled axes (the rejected ring had centre < mid -- a hole).
        self.assertGreater(centre, mid_x,
                           "centre must be strictly brighter than the "
                           "+x mid-radius pixel (no hole)")
        self.assertGreater(centre, mid_y,
                           "centre must be strictly brighter than the "
                           "+y mid-radius pixel (no hole)")
        # Mid at least as bright as the disc edge on both axes
        # (monotone non-increasing body, no rim brighter than mid).
        self.assertGreaterEqual(mid_x, disc_edge_x)
        self.assertGreaterEqual(mid_y, disc_edge_y)

        # Monotone NON-INCREASING centre->edge along +x and +y, AND no
        # interior local minimum (no r where luma rises again moving
        # outward within the solid body) -> proves there is no dark
        # hole anywhere along the radius, not just at the sample points.
        for axis in ("x", "y"):
            prev = None
            for k in range(0, 41):
                rr = r_eff * (k / 40.0)
                if axis == "x":
                    v = lum_at(cx + rr, cy)
                else:
                    v = lum_at(cx, cy + rr)
                if prev is not None:
                    self.assertLessEqual(
                        v, prev + 1e-9,
                        "luma rose moving outward at r=%.2f on %s axis "
                        "-> interior minimum / dark hole" % (rr, axis))
                prev = v

    # ---- soft glow to opaque-black background --------------------

    def test_soft_glow_monotone_to_corner(self):
        frames, palette = gen_breath.generate(
            "green", frames=FRAMES, canvas=CANVAS,
            visual_diameter=VISUAL_DIAMETER,
            scale_breath_px=SCALE_BREATH_PX)
        bi, fr = self._brightest_frame(frames, palette)
        cx = cy = (CANVAS - 1) / 2.0
        # F3: glow-monotone ray must start at the ACTUAL breathed
        # solid-disc radius of this frame, not the static nominal one.
        r_edge = self._effective_radius(bi)
        # Sample a ray from the disc edge straight out to the corner.
        samples = []
        steps = 24
        corner_x, corner_y = CANVAS - 1, CANVAS - 1
        corner_dist = math.hypot(corner_x - cx, corner_y - cy)
        # The disc-edge point on the centre->corner ray, then a
        # straight lerp from that edge point out to the corner.
        edge_frac = r_edge / corner_dist
        edge_x = cx + (corner_x - cx) * edge_frac
        edge_y = cy + (corner_y - cy) * edge_frac
        for s in range(steps + 1):
            t = s / steps  # 0 = disc edge, 1 = corner
            px = edge_x + (corner_x - edge_x) * t
            py = edge_y + (corner_y - edge_y) * t
            ix = min(CANVAS - 1, max(0, int(round(px))))
            iy = min(CANVAS - 1, max(0, int(round(py))))
            samples.append(_luma_of_index(palette, fr[iy * CANVAS + ix]))
        for a, b in zip(samples, samples[1:]):
            self.assertLessEqual(b, a + 1e-9)
        # Corner is the background and must be exactly opaque black.
        self.assertEqual(samples[-1], 0.0)

    def test_background_is_opaque_black(self):
        for name in ("blue", "green", "yellow", "red"):
            frames, palette = gen_breath.generate(
                name, frames=FRAMES, canvas=CANVAS,
                visual_diameter=VISUAL_DIAMETER,
                scale_breath_px=SCALE_BREATH_PX)
            for fr in frames:
                corner_idx = fr[0]  # top-left corner == background
                entry = palette[corner_idx * 4: corner_idx * 4 + 4]
                self.assertEqual(list(entry), [0, 0, 0, 255])

    def test_corner_fit_holds_at_breath_peak(self):
        """Corner-fit invariant across ALL 36 frames AND specifically
        at the breath PEAK (largest scale): with the ~3px scale-breath
        the disc grows by a few px, so the 32px canvas must still keep
        the grown disc + glow off all four corners (hardware-visual:
        smaller disc (20/canvas32); the grown disc + glow
        must never clip; canvas corners stay background).

        Checked two ways: (1) the geometric peak effective radius +
        _GLOW_PX must be strictly less than the centre->corner reach,
        and (2) all four actual corner pixels of every frame are the
        opaque-black background index."""
        for name in ("blue", "green", "yellow", "red"):
            frames, palette = gen_breath.generate(
                name, frames=FRAMES, canvas=CANVAS,
                visual_diameter=VISUAL_DIAMETER,
                scale_breath_px=SCALE_BREATH_PX)
            cx = cy = (CANVAS - 1) / 2.0
            corner_reach = math.hypot(cx, cy)
            # Peak effective radius = the max over the loop (the
            # brightest/largest frame). Add the constant glow halo.
            peak_r = max(self._effective_radius(fi)
                         for fi in range(FRAMES))
            self.assertLess(
                peak_r + gen_breath._GLOW_PX, corner_reach,
                "%s: grown disc+glow (%.2fpx) reaches the %dpx "
                "canvas corner (%.2fpx) -- it would clip"
                % (name, peak_r + gen_breath._GLOW_PX, CANVAS,
                   corner_reach))
            # Every frame: all four corner pixels are background and
            # the background entry is opaque black [0,0,0,255].
            corners = (0,
                       CANVAS - 1,
                       (CANVAS - 1) * CANVAS,
                       CANVAS * CANVAS - 1)
            for fr in frames:
                for ci in corners:
                    entry = palette[fr[ci] * 4: fr[ci] * 4 + 4]
                    self.assertEqual(list(entry), [0, 0, 0, 255],
                                     "%s: a canvas corner is not the "
                                     "opaque-black background" % name)

    # ---- periodic breathing (brightness envelope) ----------------

    def test_breathing_envelope_single_rise_single_fall(self):
        # Assert the single-rise/single-fall closed-loop property for
        # both the even default (36) and an odd (7) frame count: an odd
        # period has no exact-trough frame, so this guards a
        # quantisation regression the even case would hide.
        for nframes in (FRAMES, 7):
            frames, palette = gen_breath.generate(
                "red", frames=nframes, canvas=CANVAS,
                visual_diameter=VISUAL_DIAMETER,
                scale_breath_px=SCALE_BREATH_PX)
            means = [sum(_luma_of_index(palette, i) for i in fr) / len(fr)
                     for fr in frames]
            # Non-constant envelope (a real breath, not a flat ring).
            self.assertGreater(max(means) - min(means), 1.0,
                               "frames=%d" % nframes)
            # Closed loop: appending a hypothetical frame[N] equal to
            # frame[0] keeps it single-modal. Build the cyclic
            # difference signal and assert exactly one rising run and
            # one falling run.
            cyc = means + [means[0]]
            rises = _runs(cyc, lambda a, b: b > a + 1e-9)
            falls = _runs(cyc, lambda a, b: b < a - 1e-9)
            self.assertEqual(rises, 1,
                             "expected exactly one breath rise "
                             "(frames=%d)" % nframes)
            self.assertEqual(falls, 1,
                             "expected exactly one breath fall "
                             "(frames=%d)" % nframes)

    def test_breath_is_pronounced_not_subtle(self):
        """hardware-visual gate: the breath must be a
        STRONG, obviously perceptible pulse, not the prior too-subtle
        swing the user rejected on real hardware.

        Two pronounced-ness metrics, each with a threshold chosen to
        FAIL the old subtle params and PASS the new pronounced ones:

          * disc-only mean luma peak-trough delta (the brightness
            pulse measured on the lit body only, so the mostly-
            background canvas does not dilute it). Old subtle params
            (_BREATH_MIN=0.55, 28px/1.5px) measured ~30.9; the
            pronounced params (_BREATH_MIN=0.25) measure well above
            45 on the 20px disc. Threshold 45.0 sits cleanly between
            -> a regression back toward a subtle breath fails this
            test.
          * scale-breath spread in px. Old 1.5px config realised
            ~0.92px of growth; the new 3.0px default realises ~3px.
            Threshold 2.5px sits cleanly between -> a regression that
            shrinks the scale pulse fails this test (3.0px gives a
            clearly visible ~15% radius pulse on the 20px disc with a
            gentler integer step than the prior 4px).

        This is the TDD oracle for "pronounced enough": it was RED on
        the old subtle constants and is GREEN only on the new ones."""
        frames, palette = gen_breath.generate(
            "blue", frames=FRAMES, canvas=CANVAS,
            visual_diameter=VISUAL_DIAMETER,
            scale_breath_px=SCALE_BREATH_PX)
        bg = frames[0][0]

        def disc_mean(fr):
            vals = [_luma_of_index(palette, i) for i in fr if i != bg]
            return sum(vals) / len(vals) if vals else 0.0

        disc_means = [disc_mean(fr) for fr in frames]
        disc_delta = max(disc_means) - min(disc_means)
        self.assertGreaterEqual(
            disc_delta, 45.0,
            "breath mean-luma peak-trough delta %.2f is too subtle "
            "(>= 45.0 required for a pronounced, perceptible pulse; "
            "the old subtle params measured ~30.9)" % disc_delta)

        cx = cy = (CANVAS - 1) / 2.0
        radii = [self._max_nonbg_radius(fr, cx, cy) for fr in frames]
        scale_spread = max(radii) - min(radii)
        self.assertGreaterEqual(
            scale_spread, 2.5,
            "scale-breath spread %.2fpx is too subtle (>= ~2.5px "
            "required so the disc visibly grows/shrinks; the old "
            "1.5px config realised ~0.92px, the 3.0px default ~3px)"
            % scale_spread)

    # ---- scale-breath bounded ------------------------------------

    def _max_nonbg_radius(self, fr, cx, cy):
        bg = fr[0]  # top-left corner index == background for all frames
        rmax = 0.0
        for y in range(CANVAS):
            for x in range(CANVAS):
                if fr[y * CANVAS + x] != bg:
                    d = math.hypot(x - cx, y - cy)
                    if d > rmax:
                        rmax = d
        return rmax

    def test_scale_breath_bounded_and_real(self):
        cx = cy = (CANVAS - 1) / 2.0
        # A sub-1px scale breath cannot be honoured on the integer
        # pixel lattice and must be REJECTED (B1: contract must not
        # silently round it up to a full 1px pulse).
        with self.assertRaises(ValueError):
            gen_breath.generate(
                "blue", frames=FRAMES, canvas=CANVAS,
                visual_diameter=VISUAL_DIAMETER,
                scale_breath_px=0.5)
        # The bounded-and-real property must hold for several valid
        # (>= 1.0) amplitudes including the new pronounced ~3px
        # default, not a single tautological input.
        #
        # The generator breathes a WHOLE-PIXEL outer boundary
        # (grow_px in 0..scale_amp, scale_amp = floor(scale_breath_px))
        # so the boundary spread it controls is exactly scale_amp <=
        # scale_breath_px. The metric here is instead the hypot of the
        # outermost LIT lattice pixel, which -- per gen_breath's own
        # documented lattice-overshoot rationale -- jitters by up to
        # ~sqrt(2)px relative to that integer boundary depending on
        # where the boundary ring falls on the pixel grid. The upper
        # bound therefore tolerates that one-pixel-diagonal lattice
        # overshoot (it stayed a hard "<= amp" only by luck of the
        # prior 28px radius's grid phase; the smaller 26px disc shifts
        # the ring phase and exposes the artifact the generator already
        # documents). This stays a real regression oracle: the spread
        # still cannot exceed amp by more than the unavoidable single
        # diagonal pixel.
        _LATTICE = math.sqrt(2.0)  # max hypot overshoot of an integer
                                   # boundary on the pixel grid
        for amp in (1.5, 3.0, SCALE_BREATH_PX):
            frames, palette = gen_breath.generate(
                "blue", frames=FRAMES, canvas=CANVAS,
                visual_diameter=VISUAL_DIAMETER,
                scale_breath_px=amp)
            radii = [self._max_nonbg_radius(fr, cx, cy) for fr in frames]
            spread = max(radii) - min(radii)
            self.assertGreater(spread, 0.0,
                               "scale pulse must be real (amp=%s)" % amp)
            self.assertLessEqual(spread, amp + _LATTICE + 1e-9,
                                 "spread %.3f exceeds amp %s + lattice "
                                 "overshoot" % (spread, amp))

    # ---- F2: platform-deterministic scale quantisation -----------

    def test_generate_is_fully_deterministic(self):
        # Two identical calls must produce byte-identical frame data
        # AND an identical palette. Guards the committed-asset / 3MB
        # size-gate reproducibility the review flagged (F2).
        a_frames, a_pal = gen_breath.generate(
            "blue", frames=FRAMES, canvas=CANVAS,
            visual_diameter=VISUAL_DIAMETER,
            scale_breath_px=SCALE_BREATH_PX)
        b_frames, b_pal = gen_breath.generate(
            "blue", frames=FRAMES, canvas=CANVAS,
            visual_diameter=VISUAL_DIAMETER,
            scale_breath_px=SCALE_BREATH_PX)
        self.assertEqual(bytes(a_pal), bytes(b_pal))
        self.assertEqual(len(a_frames), len(b_frames))
        for fa, fb in zip(a_frames, b_frames):
            self.assertEqual(fa, fb)
        # The encoded blob must therefore also be byte-identical.
        self.assertEqual(
            bytes(eaf_encode.encode(a_frames, CANVAS, CANVAS, a_pal)),
            bytes(eaf_encode.encode(b_frames, CANVAS, CANVAS, b_pal)))

    def test_half_cycle_grow_is_documented_deterministic_value(self):
        # frames divisible by 4 -> the half-cycle frames (fi=frames/4,
        # 3*frames/4) hit the exact rounding tie. The documented
        # deterministic rule is "round half up" on grow_unit==0.5, so
        # for the default scale_breath_px=3.0 (scale_amp=floor(3.0)=3)
        # the tie value 3*0.5=1.5 rounds half up to grow_px=2, giving an
        # effective radius of base_radius + 2. FRAMES (36) is divisible
        # by 4 so the exact-tie path is still exercised after the
        # 24 -> 36 default bump (hardware-visual gate). The
        # expected grow_px is derived from scale_amp (not hardcoded) so
        # the determinism contract holds for any scale_breath_px.
        frames = FRAMES  # divisible by 4 (36)
        base_radius = VISUAL_DIAMETER / 2.0
        scale_amp = max(1, int(math.floor(SCALE_BREATH_PX)))
        # Round-half-up of the exact tie value scale_amp*0.5.
        expect_grow = math.floor(scale_amp * 0.5 + 0.5)
        for fi in (frames // 4, 3 * frames // 4):
            self.assertEqual(4 * fi % frames, 0)  # really the tie frame
            r_eff = self._effective_radius(
                fi, frames=frames, visual_diameter=VISUAL_DIAMETER,
                scale_breath_px=SCALE_BREATH_PX)
            self.assertEqual(r_eff, base_radius + expect_grow,
                             "half-cycle frame %d must use the "
                             "round-half-up tie value (grow_px=%d)"
                             % (fi, expect_grow))
        # And the recomputed effective radius must match what the
        # generator actually rendered: the pixel just inside the
        # breathed radius must be drawn (non-bg) for fi=frames/4.
        fr_list, _pal = gen_breath.generate(
            "blue", frames=frames, canvas=CANVAS,
            visual_diameter=VISUAL_DIAMETER,
            scale_breath_px=SCALE_BREATH_PX)
        cx = cy = (CANVAS - 1) / 2.0
        fi = frames // 4
        r_eff = self._effective_radius(
            fi, frames=frames, visual_diameter=VISUAL_DIAMETER,
            scale_breath_px=SCALE_BREATH_PX)
        fr = fr_list[fi]
        bg = fr[0]
        on_disc = fr[int(round(cy)) * CANVAS
                     + int(round(cx + (r_eff - 1.0)))]
        self.assertNotEqual(on_disc, bg,
                            "pixel just inside the breathed radius "
                            "must be drawn (F3/F2 consistency)")

    # ---- F4: plan edge-case coverage -----------------------------

    def test_frames_1_rejected(self):
        # A loop needs >= 2 frames; frames=1 must raise ValueError
        # (pins the documented contract, plan Task 2.1).
        with self.assertRaises(ValueError):
            gen_breath.generate(
                "blue", frames=1, canvas=CANVAS,
                visual_diameter=VISUAL_DIAMETER,
                scale_breath_px=SCALE_BREATH_PX)

    def test_scale_breath_lower_boundary_1px(self):
        # scale_breath_px == 1.0 is the lowest valid value: the pulse
        # must be real (spread > 0) and stay within the 1px integer
        # boundary plus the documented ~sqrt(2)px lattice overshoot of
        # the hypot-of-outermost-pixel metric (see the bounded-and-real
        # test for the full rationale; gen_breath documents this).
        cx = cy = (CANVAS - 1) / 2.0
        frames, _pal = gen_breath.generate(
            "blue", frames=FRAMES, canvas=CANVAS,
            visual_diameter=VISUAL_DIAMETER,
            scale_breath_px=1.0)
        radii = [self._max_nonbg_radius(fr, cx, cy) for fr in frames]
        spread = max(radii) - min(radii)
        self.assertGreater(spread, 0.0,
                           "1px scale breath must still be a real pulse")
        self.assertLessEqual(spread, 1.0 + math.sqrt(2.0) + 1e-9,
                             "1px scale breath spread %.3f exceeds 1.0 "
                             "+ lattice overshoot" % spread)

    def test_odd_canvas_generates_and_encodes(self):
        # An odd canvas (no exact integer centre) must still generate
        # valid frames that feed the real encoder and pass the basic
        # shape + opaque-black-background invariants.
        odd = 35
        frames, palette = gen_breath.generate(
            "red", frames=FRAMES, canvas=odd,
            visual_diameter=VISUAL_DIAMETER,
            scale_breath_px=SCALE_BREATH_PX)
        self.assertEqual(len(frames), FRAMES)
        for fr in frames:
            self.assertEqual(len(fr), odd * odd)
            for v in fr:
                self.assertIsInstance(v, int)
                self.assertGreaterEqual(v, 0)
                self.assertLessEqual(v, 255)
            corner = palette[fr[0] * 4: fr[0] * 4 + 4]
            self.assertEqual(list(corner), [0, 0, 0, 255])
        blob = eaf_encode.encode(frames, odd, odd, palette)
        self.assertIsInstance(blob, (bytes, bytearray))
        self.assertGreater(len(blob), 0)

    # ---- RLE-compressibility (banding) ---------------------------

    def test_rle_compressible(self):
        """The encoder default is EAF_ENCODING_RLE. A 255-step smooth
        radial+breath gradient gives almost every horizontal pixel a
        distinct palette index -> run length ~1 -> the RLE pair stream
        is LARGER than RAW (it overflows the emote partition). The
        brightness->level quantisation (``_BRIGHTNESS_LEVELS`` bands)
        turns each scanline crossing the disc into a few long equal-
        index runs so RLE compresses well.

        STRUCTURAL FLOOR FINDING: the EAF ``_S`` format
        re-emits the FULL 1024-byte palette inside every frame's
        header (eaf_encode.py:239-261 ``hdr += bytes(palette)``), so a
        36-frame asset carries 36*1024 == 36864 bytes of palette no
        matter how well the pixels compress. An ALL-BACKGROUND 36-frame
        animation already encodes to ~38KB regardless of canvas. The
        original ~15-30KB / 40000-byte target predates the 36-frame
        per-frame-palette structural floor and is BELOW it, so it is
        unreachable without changing the (out-of-scope) encoder or the
        frame count. Shrinking the canvas 40 -> 32 (hardware-visual:
        smaller disc, smaller canvas pays for the finer
        N=32 levels) cuts the per-frame pixel/pair data, so the RLE
        blob stays well under the ceiling below. The meaningful,
        in-scope gate is
        therefore: RLE must be materially SMALLER than RAW for the SAME
        frames, and far below the pre-quantisation RLE BLOAT (~104676)
        -- i.e. the banding genuinely made RLE win. The absolute
        ceiling below is set just above the encoder's palette-dominated
        floor + the banded disc's pair cost, NOT at the stale 40000.

        Asserted (each a hard size/quantisation gate):
          * RLE blob < 96000 bytes -- comfortably below the
            pre-quantisation RLE bloat (~104676) so a regression that
            removed the banding fails here, while staying above the
            unavoidable per-frame-palette structural floor (~38KB
            empty + the N=32 banded 20px-disc pair data).
          * RLE blob materially smaller than the SAME frames encoded
            RAW (proves the banding made RLE genuinely win over the
            uncompressed path, not just smaller by luck -- this is the
            real RLE-friendliness proof and is independent of the
            palette floor since RAW carries the same palettes).
          * the number of DISTINCT palette indices actually used in a
            frame is <= ``_BRIGHTNESS_LEVELS + 1`` (the +1 is the
            opaque-black background index 0) -- proves the quantisation
            stage is in effect (the un-quantised code used ~95)."""
        frames, palette = gen_breath.generate(
            "blue", frames=FRAMES, canvas=CANVAS,
            visual_diameter=VISUAL_DIAMETER,
            scale_breath_px=SCALE_BREATH_PX)
        rle = eaf_encode.encode(frames, CANVAS, CANVAS, palette)
        raw = eaf_encode.encode(frames, CANVAS, CANVAS, palette,
                                encoding=eaf_encode.EAF_ENCODING_RAW)
        self.assertLess(
            len(rle), 96000,
            "RLE blob %d bytes is not below the pre-quantisation RLE "
            "bloat -- the brightness gradient is not banded enough for "
            "RLE (see STRUCTURAL FLOOR FINDING in the docstring for why "
            "the ceiling is not the stale 40000)" % len(rle))
        self.assertLess(
            len(rle), len(raw),
            "RLE blob (%d) is not materially smaller than RAW (%d) -- "
            "the quantisation did not make it RLE-friendly"
            % (len(rle), len(raw)))
        # Distinct indices used in the brightest frame must be capped by
        # the quantisation (levels + background).
        bi, fr = self._brightest_frame(frames, palette)
        distinct = len(set(fr))
        self.assertLessEqual(
            distinct, gen_breath._BRIGHTNESS_LEVELS + 1,
            "frame uses %d distinct palette indices, expected "
            "<= _BRIGHTNESS_LEVELS+1 (%d) -- quantisation not in effect"
            % (distinct, gen_breath._BRIGHTNESS_LEVELS + 1))

    def test_quantized_still_smooth_monotone(self):
        """After quantisation the banded radial profile must still be
        monotone NON-INCREASING centre->edge (bands step DOWN, never
        up) on >= 2 axes and the centre must be the unique global max
        (the bands are concentric, no interior bright ring / no hole;
        same solid-disc contract, just discretised)."""
        frames, palette = gen_breath.generate(
            "blue", frames=FRAMES, canvas=CANVAS,
            visual_diameter=VISUAL_DIAMETER,
            scale_breath_px=SCALE_BREATH_PX)
        bi, fr = self._brightest_frame(frames, palette)
        cx = cy = (CANVAS - 1) / 2.0
        r_eff = self._effective_radius(bi)

        def lum_at(px, py):
            return _luma_of_index(
                palette, fr[int(round(py)) * CANVAS + int(round(px))])

        centre = lum_at(cx, cy)
        global_max = max(_luma_of_index(palette, i) for i in fr)
        self.assertEqual(centre, global_max,
                         "centre must remain the global max after "
                         "quantisation (concentric bands, no hole)")
        # Centre strictly the unique max vs a mid sample on both axes.
        self.assertGreater(centre, lum_at(cx + r_eff * 0.5, cy))
        self.assertGreater(centre, lum_at(cx, cy + r_eff * 0.5))
        # Banded profile still monotone non-increasing outward on +x and
        # +y: each step is <= the previous (bands step down, never up).
        for axis in ("x", "y"):
            prev = None
            for k in range(0, 41):
                rr = r_eff * (k / 40.0)
                if axis == "x":
                    v = lum_at(cx + rr, cy)
                else:
                    v = lum_at(cx, cy + rr)
                if prev is not None:
                    self.assertLessEqual(
                        v, prev + 1e-9,
                        "quantised luma rose moving outward at r=%.2f "
                        "on %s axis -> band stepped UP / interior "
                        "bright ring" % (rr, axis))
                prev = v

    # ---- temporal smoothness (anti-choppiness guard) ---------------------

    def test_breath_is_temporally_smooth_not_choppy(self):
        """hardware-visual: the user reported the breathing
        still looks choppy. Root cause: the breath is a GLOBAL
        brightness (+scale) pulse, and the per-pixel brightness is
        quantised to ``_BRIGHTNESS_LEVELS`` evenly-spaced steps. With
        too few levels the WHOLE disc only ever takes that few discrete
        brightnesses across the loop, so as the envelope sweeps the
        brightness JUMPS between those few quantised steps -> visible
        temporal stepping (the choppy "ka"), INDEPENDENT of the frame count
        (adding frames cannot interpolate between levels the palette
        does not contain).

        Oracle: the number of DISTINCT centre-pixel luma values
        exercised over the full closed loop must be a meaningful
        fraction of ``_BRIGHTNESS_LEVELS``. The CENTRE pixel is chosen
        deliberately: at the centre radial==1.0 and coverage==1.0, so
        its brightness is EXACTLY the breath envelope ``env``, then
        quantised to ``_BRIGHTNESS_LEVELS`` steps. Its distinct count
        over the loop is therefore the literal number of brightness
        plateaus the global pulse visibly steps through -- precisely
        the temporal stepping the user perceives as choppiness. (A disc-MEAN
        is a poor oracle here: averaging many radial bands smears the
        step boundaries so the mean stays ~19 distinct regardless of
        N and does NOT collapse at the coarse setting -- it would not
        catch the regression. The undiluted centre pixel does.)

        Measured: at the old _BRIGHTNESS_LEVELS=16 the centre takes
        only 12 distinct luma values over the 36-frame loop (coarse
        temporal stepping == the choppiness the user saw); at 32 it takes 16
        (roughly twice as many envelope plateaus -> half the step
        size -> smooth). The gate is a FIXED literal ``14`` chosen to
        sit strictly between those two measured counts: 12 < 14 -> RED
        at the old coarse N=16, 16 >= 14 -> GREEN at N=32. The
        threshold is deliberately NOT derived from
        ``_BRIGHTNESS_LEVELS`` -- a constant-scaled bar would drop in
        lock-step when the constant is lowered and so could never
        catch an over-coarsening regression (the exact failure mode of
        an earlier draft of this test). A future intentional change to
        the level count must consciously re-evaluate this literal.

        Deterministic: gen_breath is fully deterministic; the centre
        luma is rounded to a fixed 1e-6 grid before the distinct count
        so floating-point last-bit jitter cannot inflate it."""
        frames, palette = gen_breath.generate(
            "blue", frames=FRAMES, canvas=CANVAS,
            visual_diameter=VISUAL_DIAMETER,
            scale_breath_px=SCALE_BREATH_PX)
        cx = cy = (CANVAS - 1) / 2.0
        ci = int(round(cy)) * CANVAS + int(round(cx))

        # Round to a fixed grid so libm last-bit jitter cannot create
        # spurious "distinct" values (determinism / robustness).
        distinct_levels = len(
            {round(_luma_of_index(palette, fr[ci]), 6)
             for fr in frames})

        # FIXED gate (NOT derived from _BRIGHTNESS_LEVELS -- see the
        # docstring: a constant-scaled bar drops with the constant and
        # cannot catch an over-coarsening regression). 14 sits strictly
        # between the measured 12 (N=16, the choppy stepping) and 16 (N=32,
        # smooth): RED at the old coarse setting, GREEN at the current.
        MIN_DISTINCT_CENTRE_LEVELS = 14
        self.assertGreaterEqual(
            distinct_levels, MIN_DISTINCT_CENTRE_LEVELS,
            "breath sweeps only %d distinct centre-pixel luma values "
            "over the %d-frame loop (>= %d required: fewer means the "
            "global brightness pulse steps in too few quantised jumps "
            "-> the choppy stepping the user reported; raise "
            "_BRIGHTNESS_LEVELS, do not just add frames)"
            % (distinct_levels, FRAMES, MIN_DISTINCT_CENTRE_LEVELS))

    # ---- four colours --------------------------------------------

    def _brightest_mid_channels(self, name):
        frames, palette = gen_breath.generate(
            name, frames=FRAMES, canvas=CANVAS,
            visual_diameter=VISUAL_DIAMETER,
            scale_breath_px=SCALE_BREATH_PX)
        bi, fr = self._brightest_frame(frames, palette)
        cx = cy = (CANVAS - 1) / 2.0
        # F3: mid-radius sample uses the breathed effective radius.
        r_visual = self._effective_radius(bi)
        px = int(round(cx + r_visual * 0.5))
        py = int(round(cy))
        idx = fr[py * CANVAS + px]
        return _rgb565_truncated_channels(palette[idx * 4: idx * 4 + 4])

    def test_colour_blue_dominant(self):
        r, g, b = self._brightest_mid_channels("blue")
        self.assertGreater(b, r)
        self.assertGreater(b, g)

    def test_colour_green_dominant(self):
        r, g, b = self._brightest_mid_channels("green")
        self.assertGreater(g, r)
        self.assertGreater(g, b)

    def test_colour_red_dominant(self):
        r, g, b = self._brightest_mid_channels("red")
        self.assertGreater(r, g)
        self.assertGreater(r, b)

    def test_colour_yellow_rg_high_b_low(self):
        r, g, b = self._brightest_mid_channels("yellow")
        self.assertGreater(r, b)
        self.assertGreater(g, b)

    # ---- module-level COLOURS map --------------------------------

    def test_colours_map_present(self):
        self.assertIn("blue", gen_breath.COLOURS)
        self.assertIn("green", gen_breath.COLOURS)
        self.assertIn("yellow", gen_breath.COLOURS)
        self.assertIn("red", gen_breath.COLOURS)
        for name, rgb in gen_breath.COLOURS.items():
            self.assertEqual(len(rgb), 3)


if __name__ == "__main__":
    unittest.main()
