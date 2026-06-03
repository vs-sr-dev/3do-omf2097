#!/usr/bin/env bash
#
# build_har_atlas_sentinel.sh — build a SENTINEL (zone-distinct) CEL atlas for a
# HAR, the real fix for per-pilot fight palettes (§1.4, see
# [[project-palette-session-todo]]).
#
# Unlike build_har_atlas.sh (which reuses the shipped NEUTRAL grey CELs), this
# re-extracts the sprites with dump-all-sentinel (each armor index 1..47 gets a
# DISTINCT vivid color) and converts every frame to a CODED CEL so each frame
# carries a recolorable PLUT whose entries map 1:1 to OMF armor indices. That is
# what makes the runtime PLUT pointer-swap able to recolor each pilot.
#
# Frames are forced coded at a fixed bpp (default 6 = up to 64 colors; the
# measured worst case is 47 distinct/frame). 6bpp is the DRAM worst case — used
# for the early budget gate. Per-frame minimal bpp is a later optimization.
#
# Outputs CELs to takeme/Art/HAR_<NAME>_SENTINEL/ and the atlas to
# takeme/Art/HAR_<NAME>_SENTINEL.ATL — kept separate from the shipped neutral
# atlas so nothing working is clobbered until we promote it.
#
# Usage: ./build_har_atlas_sentinel.sh <name> <FIGHTRn.AF> <ARENAm.BK> [bpp]
#   jaguar: FIGHTR0.AF ARENA0.BK   thorn: FIGHTR2.AF ARENA2.BK
set -u

if [ "$#" -lt 3 ]; then
    echo "Usage: $0 <name> <AF_file> <BK_file> [bpp=6]"; exit 1
fi
NAME="$1"; AF="$2"; BK="$3"; BPP="${4:-6}"
NAME_UPPER=$(echo "$NAME" | tr '[:lower:]' '[:upper:]')

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
PROJ_ROOT="$(cd "$SCRIPT_DIR/.." && pwd)"
DUMP_HAR="$SCRIPT_DIR/build/dump_har"
ATLAS="$SCRIPT_DIR/build/build_atlas"
THREE_IT="$HOME/3do-devkit/bin/tools/linux/3it"
# Per-frame CELs are BUILD INTERMEDIATES — stage them in /tmp, NOT in takeme/.
# (Shipping ~600 tiny CELs per HAR balloons the 3doiso image — 3DO ISO allocates
# a large block per file; it pushed the ISO from ~100 MB to ~1.7 GB. Only the
# consolidated .ATL belongs in takeme.) The atlas is written DIRECTLY to the
# shipped path the runtime loads — no separate _SENTINEL.ATL, no promotion step.
ART_DIR="/tmp/HAR_${NAME_UPPER}_SENTINEL_cels"
ATL_OUT="$PROJ_ROOT/takeme/Art/HAR_${NAME_UPPER}.ATL"
STAGE="/tmp/${NAME}_sentinel_stage"

[ -x "$DUMP_HAR" ] || { echo "missing $DUMP_HAR (run: make -C host_tools)"; exit 1; }
[ -x "$ATLAS" ]    || { echo "missing $ATLAS (run: make -C host_tools)"; exit 1; }
[ -x "$THREE_IT" ] || { echo "missing 3it at $THREE_IT"; exit 1; }

rm -rf "$STAGE" "$ART_DIR"; mkdir -p "$STAGE" "$ART_DIR"

echo "[1/3] dump-all-sentinel $AF $BK -> $STAGE"
"$DUMP_HAR" dump-all-sentinel "$AF" "$BK" "$STAGE" || { echo "dump-all-sentinel failed"; exit 1; }

echo "[2/3] convert PNGs -> CODED ${BPP}bpp CELs"
MAN="$STAGE/atlas_manifest.txt"; : > "$MAN"
miss=0; nconv=0
while read -r line; do
    case "$line" in '#'*|'') continue ;; esac
    read -r move_id rest <<<"$line"
    mv_pad=$(printf "%02d" "$move_id")
    mf="$STAGE/move_$mv_pad/manifest.txt"
    [ -f "$mf" ] || continue
    out_dir="$ART_DIR/move_$mv_pad"; mkdir -p "$out_dir"
    while read -r mfline; do
        case "$mfline" in '#'*|'') continue ;; esac
        read -r idx fn w h px py missing <<<"$mfline"
        if [ "$missing" = "1" ]; then miss=$((miss+1)); continue; fi
        cel_base=$(basename "$fn" .png)
        png="$STAGE/move_$mv_pad/${fn}"
        cel="$out_dir/${cel_base}.CEL"
        if [ ! -f "$png" ]; then echo "WARN missing PNG: $png"; continue; fi
        # Per-frame minimal coded bpp (primary-only caps each frame at <=32
        # colors): try 4bpp packed coded, fall back to 6bpp packed coded.
        # Packed ~halves PDAT vs unpacked; coded keeps a recolorable PLUT.
        if ! "$THREE_IT" to-cel --coded true --packed true --bpp 4 \
                --output-path "$cel" "$png" > /dev/null 2>&1 || [ ! -f "$cel" ]; then
            "$THREE_IT" to-cel --coded true --packed true --bpp 6 \
                --output-path "$cel" "$png" > /dev/null 2>&1 \
                || { echo "  3it failed on $png"; continue; }
        fi
        echo "$cel" >> "$MAN"
        nconv=$((nconv+1))
    done < "$mf"
done < "$STAGE/index.txt"

nframes=$(wc -l < "$MAN")
echo "  converted $nconv CELs ($miss missing reuse-prev); $nframes atlas frames"

echo "[3/3] build atlas -> $ATL_OUT"
"$ATLAS" "$ATL_OUT" "@$MAN" || { echo "build_atlas failed"; exit 1; }
echo "wrote $ATL_OUT"
