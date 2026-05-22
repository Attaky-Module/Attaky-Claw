#!/usr/bin/env bash
#
# build_status_eaf.sh -- deterministic regen + size gate for the four
# breathing status-circle EAF assets (Task 2.2 of the status-cluster
# redesign).
#
# For each logical colour (blue/green/yellow/red) this:
#   1. calls tools.status_circle.gen_breath.generate(colour, N, C)
#   2. encodes via tools.eaf.eaf_encode.encode(...) and writes
#      components/common/emote/assets_local/emoji_large/status_<colour>.eaf
#   3. determinism self-check: regenerate into memory once more and
#      assert byte-identical (gen_breath + eaf_encode are deterministic;
#      this guards regressions)
#   4. round-trip self-check: decode the written file with the in-tree
#      decoder mirror tools/eaf/eaf_refdec.py and assert frame count == N
#      and dimensions == C (an asset the mirror cannot read is a hard
#      fail)
# then runs an a-priori size gate against the emote partition.
#
# IMPORTANT: the size gate here is an arithmetic PROXY only. It sums the
# four standalone .eaf files plus the size of the most recent
# application/edge_agent/build/emote_assets.bin.
# PROJECTED = current_bin + sum(new .eaf) is intentionally a
# conservative OVER-estimate (the real packer recompacts and the status
# assets are new, not duplicated), so a PASS here is trustworthy. The
# packer recompacts assets, so the real authority is the controller's
# `stat` of the rebuilt emote_assets.bin after `idf.py gen-bmgr-config`
# + `idf.py build`. This script never runs idf.py / flash / serial.
#
# Deterministic + re-runnable. English / ASCII only. No AI attribution.

set -euo pipefail

# ---- tunables -------------------------------------------------------
# Pinned to the gen_breath solid-disc defaults: a 32px tight canvas
# (visual_diameter 20 + glow + scale headroom) and a 36-frame closed
# loop. hardware-visual: smaller disc (20/canvas32) + finer
# brightness (N=32) to remove temporal banding (the choppy stepping);
# smaller canvas pays the size for more levels; scale 3px gentler
# integer step. The disc
# shrank 26 -> 20 (user asked for a smaller circle) and the canvas
# 40 -> 32 to fit the 20px disc + glow + the gentler ~3px scale-breath
# while cutting the per-frame RLE index data so the extra brightness
# levels stay roughly size-neutral. 36 frames give fine steps and with
# the engine fps 12 a 36/12 = 3.0s breathing cycle. Must match
# gen_breath.generate() defaults.
FRAMES=36          # gen_breath default frame count (closed loop)
CANVAS=32          # gen_breath default square canvas size in px

# ---- size-gate constants -------------------------------------------
# emote partition size in bytes. Source: the `emote` row in
# application/edge_agent/partitions_8MB.csv and partitions_16MB.csv
# (`emote, data, spiffs, , 3M` => 3 * 1024 * 1024 = 3145728).
EMOTE_PARTITION=3145728
SIZE_MARGIN=65536           # 64 KiB explicit safety margin held back

# ---- derive the worktree root from this script's location ----------
# (script lives at <root>/tools/status_circle/build_status_eaf.sh)
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ROOT="$(cd "${SCRIPT_DIR}/../.." && pwd)"
cd "${ROOT}"

ASSET_DIR="components/common/emote/assets_local/emoji_large"
EMOTE_BIN="application/edge_agent/build/emote_assets.bin"

COLOURS="blue green yellow red"

echo "=== build_status_eaf.sh ==="
echo "root        : ${ROOT}"
echo "frames (N)  : ${FRAMES}"
echo "canvas (C)  : ${CANVAS}"
echo "asset dir   : ${ASSET_DIR}"
echo

mkdir -p "${ASSET_DIR}"

# Generate + encode + self-checks for every colour. The Python helper
# does generate -> encode -> write -> determinism re-encode -> refdec
# round-trip, and exits nonzero on any failure.
for colour in ${COLOURS}; do
    out="${ASSET_DIR}/status_${colour}.eaf"
    FRAMES="${FRAMES}" CANVAS="${CANVAS}" OUT="${out}" COLOUR="${colour}" \
        python3 - <<'PY'
import os
import sys

from tools.status_circle import gen_breath
from tools.eaf import eaf_encode, eaf_refdec

colour = os.environ["COLOUR"]
out = os.environ["OUT"]
N = int(os.environ["FRAMES"])
C = int(os.environ["CANVAS"])

# 1. generate + encode (pass A).
frames, palette = gen_breath.generate(colour, frames=N, canvas=C)
blob = eaf_encode.encode(frames, C, C, palette)

with open(out, "wb") as fh:
    fh.write(blob)

# 2. determinism self-check: a second independent generate+encode must
#    be byte-identical (deterministic tooling; guards regressions).
frames2, palette2 = gen_breath.generate(colour, frames=N, canvas=C)
blob2 = eaf_encode.encode(frames2, C, C, palette2)
if blob2 != blob:
    sys.stderr.write(
        "DETERMINISM FAIL: %s second encode differs (%d vs %d bytes)\n"
        % (colour, len(blob2), len(blob)))
    sys.exit(1)

# 3. round-trip self-check: the in-tree decoder mirror must parse the
#    written file with frame count == N and dimensions == C.
with open(out, "rb") as fh:
    on_disk = fh.read()
container, decoded = eaf_refdec.decode(on_disk)
if container["total_frames"] != N or len(decoded) != N:
    sys.stderr.write(
        "ROUNDTRIP FAIL: %s frame count %d / %d != N=%d\n"
        % (colour, container["total_frames"], len(decoded), N))
    sys.exit(1)
for fi, fr in enumerate(decoded):
    if fr["width"] != C or fr["height"] != C:
        sys.stderr.write(
            "ROUNDTRIP FAIL: %s frame %d dims %dx%d != %dx%d\n"
            % (colour, fi, fr["width"], fr["height"], C, C))
        sys.exit(1)

sz = len(blob)
print("  %-7s -> %s  (%d bytes, det OK, refdec OK: %d frames %dx%d)"
      % (colour, out, sz, N, C, C))
PY
done

echo
echo "=== size gate (a-priori proxy; controller stat of rebuilt bin is final) ==="

# Sum the four written .eaf and read the current emote_assets.bin size.
NEW_SUM=0
declare -a SIZES
for colour in ${COLOURS}; do
    f="${ASSET_DIR}/status_${colour}.eaf"
    s=$(wc -c < "${f}")
    SIZES+=("${colour}:${s}")
    NEW_SUM=$(( NEW_SUM + s ))
done

if [ -f "${EMOTE_BIN}" ]; then
    CUR_BIN=$(wc -c < "${EMOTE_BIN}")
else
    CUR_BIN=0
    echo "WARNING: ${EMOTE_BIN} not present; treating current bin size as 0."
fi

PROJECTED=$(( CUR_BIN + NEW_SUM ))
BUDGET=$(( EMOTE_PARTITION - SIZE_MARGIN ))
HEADROOM=$(( EMOTE_PARTITION - PROJECTED ))

echo
echo "  --- per-file ---"
for entry in "${SIZES[@]}"; do
    name="${entry%%:*}"
    val="${entry##*:}"
    printf "  status_%-7s %12d B\n" "${name}" "${val}"
done
echo "  ----------------------------------------"
printf "  sum new .eaf            %12d B\n" "${NEW_SUM}"
printf "  current emote_assets.bin%12d B\n" "${CUR_BIN}"
printf "  projected (sum+bin)     %12d B\n" "${PROJECTED}"
printf "  emote partition         %12d B\n" "${EMOTE_PARTITION}"
printf "  safety margin           %12d B\n" "${SIZE_MARGIN}"
printf "  budget (part - margin)  %12d B\n" "${BUDGET}"
printf "  headroom (part - proj)  %12d B\n" "${HEADROOM}"
echo

if [ "${PROJECTED}" -gt "${BUDGET}" ]; then
    echo "SIZE GATE: FAIL -- projected ${PROJECTED} B > budget ${BUDGET} B"
    echo "  (partition ${EMOTE_PARTITION} B minus margin ${SIZE_MARGIN} B)"
    exit 1
fi

echo "SIZE GATE: PASS -- projected ${PROJECTED} B <= budget ${BUDGET} B"
echo "=== done ==="
