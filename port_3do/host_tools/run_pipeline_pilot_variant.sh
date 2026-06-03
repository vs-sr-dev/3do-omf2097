#!/usr/bin/env bash
: "${OMF_DATA:?set OMF_DATA to your OMF:2097 data directory (e.g. export OMF_DATA=/path/to/OMF)}"
#
# run_pipeline_pilot_variant.sh -- extract a pilot-altpal variant of an
# already-pipelined HAR. Only the CEL files at Art/HAR_<HAR>_<PILOT>/
# are emitted; the C data tables (jaguar_all_data.h etc.) are NOT
# regenerated -- the runtime swaps the CEL path prefix at LoadCel time
# when (gSelectedPilotId, gSelectedHarId) match.
#
# Sessione 8 PoC for the per-pilot HAR coloring system (per
# [[project-har-palette-remap]]). For sessione 8 we only generate
# Jaguar+CRYSTAL as proof; other pilot x HAR combos can be added in
# subsequent sessions by repeating this script with different args.
#
# Usage:
#   ./run_pipeline_pilot_variant.sh <har_name> <pilot_name> <pilot_id> \
#                                   <FIGHTRn.AF> <ARENA0.BK> <ALTPALS.DAT>
# Example:
#   ./run_pipeline_pilot_variant.sh jaguar crystal 0 \
#       ${OMF_DATA}/FIGHTR0.AF ${OMF_DATA}/ARENA0.BK ${OMF_DATA}/ALTPALS.DAT

set -u

if [ "$#" -ne 6 ]; then
    echo "Usage: $0 <har_name> <pilot_name> <pilot_id 0..9> <FIGHTR.AF> <ARENA.BK> <ALTPALS.DAT>" >&2
    exit 1
fi

HAR="$1"
PILOT="$2"
PILOT_ID="$3"
AF="$4"
BK="$5"
ALTPALS="$6"
HAR_UPPER=$(echo "$HAR"   | tr '[:lower:]' '[:upper:]')
PILOT_UPPER=$(echo "$PILOT" | tr '[:lower:]' '[:upper:]')

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
PROJ_ROOT="$(cd "$SCRIPT_DIR/.." && pwd)"
DUMP_HAR="$SCRIPT_DIR/build/dump_har"
THREE_IT="$HOME/3do-devkit/bin/tools/linux/3it"

STAGE_DIR="/tmp/${HAR}_${PILOT}_stage"
ART_DIR="$PROJ_ROOT/takeme/Art/HAR_${HAR_UPPER}_${PILOT_UPPER}"

rm -rf "$STAGE_DIR" "$ART_DIR"
mkdir -p "$STAGE_DIR" "$ART_DIR"

echo "[1/2] dump-all-pilot $AF $BK $ALTPALS pilot=$PILOT_ID -> $STAGE_DIR"
"$DUMP_HAR" dump-all-pilot "$AF" "$BK" "$ALTPALS" "$PILOT_ID" "$STAGE_DIR" \
    > /dev/null || { echo "dump-all-pilot failed"; exit 1; }
n_moves=$(grep -c '^[0-9]' "$STAGE_DIR/index.txt" || echo 0)
echo "  dumped $n_moves moves"

echo "[2/2] convert PNGs to CELs -> $ART_DIR"
total_cels=0
total_bytes=0
while read -r line; do
    case "$line" in '#'*|'') continue ;; esac
    read -r move_id cat sprite_count mstring rest <<<"$line"
    mv_pad=$(printf "%02d" "$move_id")
    out_dir="$ART_DIR/move_$mv_pad"
    mkdir -p "$out_dir"
    for png in "$STAGE_DIR/move_$mv_pad"/frame_*.png; do
        [ -f "$png" ] || continue
        base=$(basename "$png" .png)
        out="$out_dir/${base}.CEL"
        "$THREE_IT" to-cel --find-smallest regular --output-path "$out" "$png" \
            > /dev/null 2>&1 || { echo "  3it failed on $png"; continue; }
        total_cels=$((total_cels + 1))
        total_bytes=$((total_bytes + $(stat -c%s "$out")))
    done
done < "$STAGE_DIR/index.txt"
echo "  converted $total_cels CELs, total $((total_bytes / 1024)) KB"
echo "Pipeline OK."
echo "  $ART_DIR/"
