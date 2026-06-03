#!/usr/bin/env bash
# Visualize a few glyphs from the dump_font stage dir as ASCII art.
# # = opaque white, . = transparent. Shows the actual 6x6 glyph region
# inside the 8x8 padded cell.
set -u

STAGE="${1:-/tmp/font_small_stage}"

for ch in 32 33 65 66 87 105 119; do
    echo "=== ASCII $ch ==="
    # convert dumps lines like "X,Y: (R,G,B,A)  #RRGGBBAA  name".
    # awk: extract X,Y from $1 by stripping the trailing ":" and splitting.
    convert "$STAGE/g${ch}.png" txt: | tail -n +2 | awk '
        {
            coord = $1
            sub(/:$/, "", coord)
            split(coord, p, ",")
            x = p[1] + 0
            y = p[2] + 0
            if (y < 6) {
                if ($3 ~ /^#FFFFFFFF/) printf "#"; else printf "."
                if (x == 7) printf "\n"
            }
        }'
done
