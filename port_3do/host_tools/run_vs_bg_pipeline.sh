#!/usr/bin/env bash
: "${OMF_DATA:?set OMF_DATA to your OMF:2097 data directory (e.g. export OMF_DATA=/path/to/OMF)}"
#
# run_vs_bg_pipeline.sh — host pipeline for VS.BK pre-fight background.
#
# Differs from run_bg_pipeline.sh: VS.BK raw bg has the canon hangar on
# the LEFT half (x=0..159) and the financial-report wireframe on the
# RIGHT half (x=160..319). For a normal p1-vs-p2 fight the canon code
# (openomf-master/src/game/scenes/vs.c L647-656) replaces the right half
# with a horizontally-flipped copy of the left half at runtime. We do
# the same at pipeline time so the runtime CEL is canon-correct as-is.
#
# Steps:
#   1. dump_har bg     → PNG of bk->background using bk->palettes[0]
#   2. ImageMagick     → mirror left half onto right half
#   3. 3it to-cel      → one CEL into takeme/Art/VS.CEL
#
# Usage:
#   ./run_vs_bg_pipeline.sh <VS.BK>
# Example:
#   ./run_vs_bg_pipeline.sh ${OMF_DATA}/VS.BK

set -u

if [ "$#" -ne 1 ]; then
    echo "Usage: $0 <VS.BK_file>"
    exit 1
fi

BK="$1"
NAME="VS"

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
PROJ_ROOT="$(cd "$SCRIPT_DIR/.." && pwd)"
DUMP_HAR="$SCRIPT_DIR/build/dump_har"
THREE_IT="$HOME/3do-devkit/bin/tools/linux/3it"

STAGE_DIR="/tmp/${NAME,,}_bg_stage"
CEL_OUT="$PROJ_ROOT/takeme/Art/${NAME}.CEL"

mkdir -p "$STAGE_DIR" "$(dirname "$CEL_OUT")"

echo "[1/3] dump_har bg $BK -> $STAGE_DIR/bg.png"
"$DUMP_HAR" bg "$BK" "$STAGE_DIR" || { echo "dump_har failed"; exit 1; }

echo "[2/3] mirror left half (0..159) onto right half (160..319)"
convert "$STAGE_DIR/bg.png" \
    \( -clone 0 -crop 160x200+0+0 +repage -flop \) \
    -geometry +160+0 -composite \
    "$STAGE_DIR/bg_mirrored.png" \
    || { echo "ImageMagick mirror FAILED"; exit 1; }

echo "[3/3] 3it to-cel -> $CEL_OUT"
"$THREE_IT" to-cel --find-smallest regular --output-path "$CEL_OUT" \
    "$STAGE_DIR/bg_mirrored.png" \
    > /dev/null 2>&1 \
    && echo "  bg_mirrored.PNG -> ${NAME}.CEL ($(stat -c%s "$CEL_OUT") bytes)" \
    || { echo "  3it FAILED"; exit 1; }

echo "Pipeline OK."
