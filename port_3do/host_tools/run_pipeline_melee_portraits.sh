#!/usr/bin/env bash
: "${OMF_DATA:?set OMF_DATA to your OMF:2097 data directory (e.g. export OMF_DATA=/path/to/OMF)}"
#
# run_pipeline_melee_portraits.sh -- extract MELEE.BK portrait sheets +
# split into per-cell CELs for the canon select-screen rendering.
#
# Canon MELEE.BK animation map (verified 2026-05-21 via dump_bk_bg info
# + visual inspection of anim[0]..anim[5] + user-observed runtime test):
#   anim[0]  -> 1 sprite 299x78  -- grayscale PILOT portrait grid (5x2)
#                                   (was previously misread as HAR sheet)
#   anim[1]  -> 1 sprite 299x78  -- COLOR HAR portrait grid (5x2)
#   anim[3]  -> 10 sprites 51x36 -- small color PILOT portraits (Crystal..Raven)
#   anim[4]  -> 11 sprites ~57x57 -- big PILOT face portraits (10 = NOVA)
#   anim[5]  -> 2 sprites top-right -- ONE MUST FALL 2097 logo + variants
#
# DOS canon select-screen behavior (from ground-truth screenshots): the
# bottom grid switches semantics by page:
#   PILOT_PAGE: anim[0] in non-selected cells (gray pilots) + anim[3]
#               sprite N in selected cell with red BG.
#   HAR_PAGE:   anim[1]-DIMMED in non-selected (gray HAR look) + anim[1]
#               full-bright in selected + red BG.
# We pre-generate the HAR dim variant via ImageMagick (modulate 45,0)
# since the BK doesn't ship a HAR-grayscale-only sheet -- the DOS game
# applies the dim treatment at runtime.
#
# Outputs into takeme/Art/PORTRAITS/:
#   har_sheet_color.CEL         (anim[1], single 299x78)  -- kept for debug
#   har_sheet_gray.CEL          (anim[0], single 299x78)  -- kept for debug
#   har_color_NN.CEL  (N=0..9)  per-cell colored HAR portrait
#   har_gray_NN.CEL   (N=0..9)  per-cell grayscale HAR portrait
#   pilot_small_NN.CEL (N=0..9) anim[3] small pilot face
#   pilot_big_NN.CEL   (N=0..10) anim[4] big pilot face
#   logo_NN.CEL        (N=0..1) anim[5] menu logo sprites
#   red_cell.CEL                51x36 solid red (selected-cell BG)
#
# Header:
#   src/portraits_data.h -- exposes har_color[N]/har_gray[N]/pilot_*[N]/logo[N]/red_cell

set -u

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
PROJ_ROOT="$(cd "$SCRIPT_DIR/.." && pwd)"
DUMP="$SCRIPT_DIR/build/dump_bk_bg"
THREE_IT="$HOME/3do-devkit/bin/tools/linux/3it"

BK="${1:-${OMF_DATA}/MELEE.BK}"
A0_STAGE="/tmp/melee_a0_stage"
A1_STAGE="/tmp/melee_a1_stage"
A3_STAGE="/tmp/melee_a3_stage"
A4_STAGE="/tmp/melee_a4_stage"
A5_STAGE="/tmp/melee_a5_stage"
RED_STAGE="/tmp/melee_red_stage"
CEL_DIR="$PROJ_ROOT/takeme/Art/PORTRAITS"
HEADER="$PROJ_ROOT/src/portraits_data.h"

if [ ! -x "$DUMP" ]; then
    echo "dump_bk_bg not built; run 'make build/dump_bk_bg' first." >&2
    exit 1
fi

rm -rf "$A0_STAGE" "$A1_STAGE" "$A3_STAGE" "$A4_STAGE" "$A5_STAGE" \
       "$RED_STAGE" "$CEL_DIR"
mkdir -p "$A0_STAGE" "$A1_STAGE" "$A3_STAGE" "$A4_STAGE" "$A5_STAGE" \
         "$RED_STAGE" "$CEL_DIR"

echo "[1/6] dump_bk_bg dump-anim 0/1/3/4/5"
"$DUMP" dump-anim "$BK" 0 "$A0_STAGE" > /dev/null \
    || { echo "dump-anim 0 failed"; exit 1; }
"$DUMP" dump-anim "$BK" 1 "$A1_STAGE" > /dev/null \
    || { echo "dump-anim 1 failed"; exit 1; }
"$DUMP" dump-anim "$BK" 3 "$A3_STAGE" > /dev/null \
    || { echo "dump-anim 3 failed"; exit 1; }
"$DUMP" dump-anim "$BK" 4 "$A4_STAGE" > /dev/null \
    || { echo "dump-anim 4 failed"; exit 1; }
"$DUMP" dump-anim "$BK" 5 "$A5_STAGE" > /dev/null \
    || { echo "dump-anim 5 failed"; exit 1; }

echo "[2/6] split anim[0]/anim[1] sheets into 10 per-cell PNGs (pitch 62x42)"
# Sheets are 299x78, cells 51x36 at pitch (62 horizontal, 42 vertical),
# 5 cols x 2 rows. ImageMagick crop is row-major (col-by-col, row-by-row).
# We name them N = row*5 + col so it matches the HAR enum (0=JAGUAR,
# 5=KATANA, 9=CHRONOS).
split_sheet() {
    local sheet="$1"     # sprite_00.png 299x78
    local out_dir="$2"
    local prefix="$3"    # e.g. "har_color_" or "har_gray_"
    local col row x y
    for row in 0 1; do
        for col in 0 1 2 3 4; do
            x=$(( col * 62 ))
            y=$(( row * 42 ))
            local idx=$(( row * 5 + col ))
            convert "$sheet" -crop 51x36+${x}+${y} +repage \
                "$out_dir/${prefix}$(printf "%02d" $idx).png"
        done
    done
}

split_sheet "$A0_STAGE/sprite_00.png" "$A0_STAGE" "pilot_gray_"
split_sheet "$A1_STAGE/sprite_00.png" "$A1_STAGE" "har_color_"

# Pre-generate dim/grayscale HAR cells from anim[1] color (canon DOS does
# this at runtime; we bake it host-side to skip the 3DO CCB PIXC dance).
# -modulate brightness,saturation: 45% bright, 0% sat = darker grayscale.
for i in 0 1 2 3 4 5 6 7 8 9; do
    iz=$(printf "%02d" $i)
    convert "$A1_STAGE/har_color_${iz}.png" -modulate 45,0 \
        "$A1_STAGE/har_dim_${iz}.png"
done

echo "[3/6] generate red_cell.png (51x36 solid red selected-cell BG)"
# Slightly desaturated red so it doesn't clash; the canon "pulsing" red BG
# does brightness oscillation -- we just pick mid-bright. ImageMagick xc:
# names use SVG-ish colors.
convert -size 51x36 xc:#9c2018 "$RED_STAGE/red_cell.png"

echo "[4/6] 3it to-cel for every PNG"
ok_total=0

# Convert helper: $1=png, $2=cel
to_cel() {
    "$THREE_IT" to-cel --find-smallest regular --output-path "$2" "$1" \
            > /dev/null 2>&1 \
        && ok_total=$((ok_total + 1)) \
        || echo "  3it failed on $1"
}

to_cel "$A0_STAGE/sprite_00.png"  "$CEL_DIR/pilot_gray_sheet.CEL"
to_cel "$A1_STAGE/sprite_00.png"  "$CEL_DIR/har_color_sheet.CEL"

for png in "$A0_STAGE"/pilot_gray_*.png; do
    base=$(basename "$png" .png)
    to_cel "$png" "$CEL_DIR/${base}.CEL"
done
for png in "$A1_STAGE"/har_color_*.png; do
    base=$(basename "$png" .png)
    to_cel "$png" "$CEL_DIR/${base}.CEL"
done
for png in "$A1_STAGE"/har_dim_*.png; do
    base=$(basename "$png" .png)
    to_cel "$png" "$CEL_DIR/${base}.CEL"
done
for png in "$A3_STAGE"/sprite_*.png; do
    base=$(basename "$png" .png)
    idx="${base#sprite_}"
    to_cel "$png" "$CEL_DIR/pilot_small_${idx}.CEL"
done
for png in "$A4_STAGE"/sprite_*.png; do
    base=$(basename "$png" .png)
    idx="${base#sprite_}"
    to_cel "$png" "$CEL_DIR/pilot_big_${idx}.CEL"
done
for png in "$A5_STAGE"/sprite_*.png; do
    base=$(basename "$png" .png)
    idx="${base#sprite_}"
    to_cel "$png" "$CEL_DIR/logo_${idx}.CEL"
done
to_cel "$RED_STAGE/red_cell.png" "$CEL_DIR/red_cell.CEL"
echo "  $ok_total CELs written"

echo "[5/6] write C header -> $HEADER"

emit_anim_entries() {
    local manifest="$1"
    local prefix="$2"     # e.g. "pilot_small_"
    awk -v prefix="$prefix" '
        $1 ~ /^[0-9]+$/ {
            if ($2 == "-") { next; }
            idx = $1; w = $3; h = $4; px = $5; py = $6;
            printf("    { %d, %d, %d, %d, %d, \"Art/PORTRAITS/%s%02d.CEL\" },\n",
                   idx, w, h, px, py, prefix, idx);
        }
    ' "$manifest"
}

emit_grid_per_cell() {
    local prefix="$1"     # har_color_ / har_dim_ / pilot_gray_
    local positions_x=(11 73 135 197 259  11 73 135 197 259)
    local positions_y=(115 115 115 115 115  157 157 157 157 157)
    local i
    for i in 0 1 2 3 4 5 6 7 8 9; do
        printf "    { %d, 51, 36, %d, %d, \"Art/PORTRAITS/%s%02d.CEL\" },\n" \
            "$i" "${positions_x[$i]}" "${positions_y[$i]}" "$prefix" "$i"
    done
}

{
    echo "/* Auto-generated by port_3do/host_tools/run_pipeline_melee_portraits.sh -- do NOT edit. */"
    echo "/* source: $BK */"
    echo "#ifndef PORTRAITS_DATA_H"
    echo "#define PORTRAITS_DATA_H"
    echo ""
    echo "typedef struct {"
    echo "    int         id;            /* sprite index inside its MELEE.BK animation */"
    echo "    int         w, h;          /* portrait pixel dims */"
    echo "    int         pos_x, pos_y;  /* OMF anchor position */"
    echo "    const char *cel_path;      /* 3DO filesystem path */"
    echo "} portrait_data_t;"
    echo ""
    echo "/* MELEE.BK anim[1] color HAR portraits per-cell (selected look). */"
    echo "static const portrait_data_t har_color_cells[] = {"
    emit_grid_per_cell "har_color_"
    echo "};"
    echo "/* anim[1]-derived darkened/desaturated variant (non-selected look on HAR page). */"
    echo "static const portrait_data_t har_dim_cells[] = {"
    emit_grid_per_cell "har_dim_"
    echo "};"
    echo "/* MELEE.BK anim[0] -- per-cell GRAYSCALE pilot portraits (non-selected look"
    echo " * on PILOT page; the previous misread as 'gray HAR' was a sessione 7-bug)."
    echo " * Kept available for future per-page diffing. */"
    echo "static const portrait_data_t pilot_gray_cells[] = {"
    emit_grid_per_cell "pilot_gray_"
    echo "};"
    echo "#define HAR_CELLS_COUNT  (sizeof(har_color_cells) / sizeof(har_color_cells[0]))"
    echo ""
    echo "/* MELEE.BK anim[3] -- 10 small PILOT portraits 51x36. */"
    echo "static const portrait_data_t pilot_small_portraits[] = {"
    emit_anim_entries "$A3_STAGE/manifest.txt" "pilot_small_"
    echo "};"
    echo "#define PILOT_SMALL_COUNT  (sizeof(pilot_small_portraits) / sizeof(pilot_small_portraits[0]))"
    echo ""
    echo "/* MELEE.BK anim[4] -- 11 big PILOT face portraits (sprite 10 = NOVA). */"
    echo "static const portrait_data_t pilot_big_portraits[] = {"
    emit_anim_entries "$A4_STAGE/manifest.txt" "pilot_big_"
    echo "};"
    echo "#define PILOT_BIG_COUNT  (sizeof(pilot_big_portraits) / sizeof(pilot_big_portraits[0]))"
    echo ""
    echo "/* MELEE.BK anim[5] -- menu logo sprites (top-right of select screen). */"
    echo "static const portrait_data_t logo_sprites[] = {"
    emit_anim_entries "$A5_STAGE/manifest.txt" "logo_"
    echo "};"
    echo "#define LOGO_COUNT  (sizeof(logo_sprites) / sizeof(logo_sprites[0]))"
    echo ""
    echo "/* Solid red cell used as a BG behind the selected grid slot. 51x36 fixed. */"
    echo "#define RED_CELL_PATH  \"Art/PORTRAITS/red_cell.CEL\""
    echo "#define RED_CELL_W     51"
    echo "#define RED_CELL_H     36"
    echo ""
    echo "#endif /* PORTRAITS_DATA_H */"
} > "$HEADER"

echo "Pipeline OK."
echo "  $CEL_DIR/"
echo "  $HEADER"
