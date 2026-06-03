#!/usr/bin/env bash
# gen_menu_frame_cels.sh -- one-shot CEL generator for the main-menu dialog
# frame. Produces takeme/Art/menu_frame.CEL.
#
# Parity with openomf's main-menu gui_frame: the canon menu lives in a dark
# bordered dialog box placed at (165,5) size 151x119 in OMF's 320x200 canvas
# (openomf-master/src/game/scenes/mainmenu.c:123). We bake the box at its
# EXACT pixel size so the runtime draws it 1:1 with no HDX/VDY scaling (avoids
# edge-stretch artifacts that a scaled solid CEL would show).
#
# Colors approximate the canon dark dialog: deep navy fill, light-grey border.
# The frame is opaque (no alpha blend) -- the 3DO menu sits over a flat clear
# color, so opacity reads fine and keeps the CEL format simple.
set -e

THREE_IT="$HOME/3do-devkit/bin/tools/linux/3it"
SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
PROJ="$(cd "$SCRIPT_DIR/.." && pwd)"
ART="$PROJ/takeme/Art"
TMP="/tmp/menu_frame_gen"

mkdir -p "$TMP" "$ART"

# 151x119 dark-navy fill with a 2px light-grey border drawn inside the edge.
convert -size 151x119 xc:"#0E1430" \
        -fill none -stroke "#8898C0" -strokewidth 2 \
        -draw "rectangle 1,1 149,117" \
        "$TMP/menu_frame.png"

"$THREE_IT" to-cel --find-smallest regular \
    --output-path "$ART/menu_frame.CEL" "$TMP/menu_frame.png"

ls -la "$ART"/menu_frame.CEL
