#!/usr/bin/env bash
# gen_hpbar_cels.sh -- one-shot CEL generator for the battle health bars.
# Produces takeme/Art/hpbar_{bg,fg}.CEL (100x6 px solid color). Runtime
# scales the FG horizontally via ccb_HDX to depict remaining HP %.
set -e

THREE_IT="$HOME/3do-devkit/bin/tools/linux/3it"
SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
PROJ="$(cd "$SCRIPT_DIR/.." && pwd)"
ART="$PROJ/takeme/Art"
TMP="/tmp/hpbar_gen"

mkdir -p "$TMP" "$ART"

convert -size 100x6 xc:"#202020" "$TMP/hpbar_bg.png"
convert -size 100x6 xc:"#E03030" "$TMP/hpbar_fg.png"

"$THREE_IT" to-cel --find-smallest regular --output-path "$ART/hpbar_bg.CEL" "$TMP/hpbar_bg.png"
"$THREE_IT" to-cel --find-smallest regular --output-path "$ART/hpbar_fg.CEL" "$TMP/hpbar_fg.png"

ls -la "$ART"/hpbar_*.CEL
