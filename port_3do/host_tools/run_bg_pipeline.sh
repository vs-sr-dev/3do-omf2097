#!/usr/bin/env bash
: "${OMF_DATA:?set OMF_DATA to your OMF:2097 data directory (e.g. export OMF_DATA=/path/to/OMF)}"
#
# run_bg_pipeline.sh — host pipeline for BK arena backgrounds.
#
# Steps:
#   1. dump_har bg     → PNG of bk->background using bk->palettes[0]
#   2. 3it to-cel      → one CEL into takeme/Art/
#
# No C header is emitted — the runtime hardcodes the CEL path as
# "Art/<NAME>.CEL". See hello.c.
#
# Usage:
#   ./run_bg_pipeline.sh <arena_name> <ARENA*.BK>
# Example:
#   ./run_bg_pipeline.sh ARENA0 ${OMF_DATA}/ARENA0.BK

set -u

if [ "$#" -ne 2 ]; then
    echo "Usage: $0 <arena_name> <BK_file>"
    exit 1
fi

NAME="$1"
BK="$2"

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
PROJ_ROOT="$(cd "$SCRIPT_DIR/.." && pwd)"
DUMP_HAR="$SCRIPT_DIR/build/dump_har"
THREE_IT="$HOME/3do-devkit/bin/tools/linux/3it"

STAGE_DIR="/tmp/${NAME,,}_bg_stage"
CEL_OUT="$PROJ_ROOT/takeme/Art/${NAME}.CEL"

mkdir -p "$STAGE_DIR" "$(dirname "$CEL_OUT")"

echo "[1/2] dump_har bg $BK -> $STAGE_DIR/bg.png"
"$DUMP_HAR" bg "$BK" "$STAGE_DIR" || { echo "dump_har failed"; exit 1; }

echo "[2/2] 3it to-cel -> $CEL_OUT"
"$THREE_IT" to-cel --find-smallest regular --output-path "$CEL_OUT" "$STAGE_DIR/bg.png" \
    > /dev/null 2>&1 \
    && echo "  bg.PNG -> ${NAME}.CEL ($(stat -c%s "$CEL_OUT") bytes)" \
    || { echo "  3it FAILED"; exit 1; }

echo "Pipeline OK."
