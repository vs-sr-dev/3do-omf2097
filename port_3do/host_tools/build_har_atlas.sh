#!/usr/bin/env bash
#
# build_har_atlas.sh — build a CEL atlas (.ATL) for a HAR from its EXISTING
# per-frame CELs, in the exact runtime moves[] x frames[] non-missing order.
#
# Reuses `dump_har dump-all` purely for the move/frame ORDER and the missing
# flags (the same source the committed <name>_all_data.h was generated from),
# so the atlas frame index lines up 1:1 with LoadHarFromAtlas's walk. Does NOT
# re-convert CELs or regenerate the C headers — fast and low-risk.
#
# Usage: ./build_har_atlas.sh <name> <FIGHTRn.AF> <ARENAm.BK>
#   jaguar: FIGHTR0.AF ARENA0.BK   thorn: FIGHTR2.AF ARENA2.BK
set -u

if [ "$#" -ne 3 ]; then
    echo "Usage: $0 <name> <AF_file> <BK_file>"; exit 1
fi
NAME="$1"; AF="$2"; BK="$3"
NAME_UPPER=$(echo "$NAME" | tr '[:lower:]' '[:upper:]')

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
PROJ_ROOT="$(cd "$SCRIPT_DIR/.." && pwd)"
DUMP_HAR="$SCRIPT_DIR/build/dump_har"
ATLAS="$SCRIPT_DIR/build/build_atlas"
ART_DIR="$PROJ_ROOT/takeme/Art/HAR_${NAME_UPPER}"
ATL_OUT="$PROJ_ROOT/takeme/Art/HAR_${NAME_UPPER}.ATL"
STAGE="/tmp/${NAME}_atlas_stage"

[ -x "$DUMP_HAR" ] || { echo "missing $DUMP_HAR (run: make -C host_tools)"; exit 1; }
[ -x "$ATLAS" ]    || { echo "missing $ATLAS (run: make -C host_tools)"; exit 1; }
[ -d "$ART_DIR" ]  || { echo "missing CELs dir $ART_DIR"; exit 1; }

rm -rf "$STAGE"; mkdir -p "$STAGE"
"$DUMP_HAR" dump-all "$AF" "$BK" "$STAGE" > /dev/null || { echo "dump-all failed"; exit 1; }

MAN="$STAGE/atlas_manifest.txt"; : > "$MAN"
miss=0
while read -r line; do
    case "$line" in '#'*|'') continue ;; esac
    read -r move_id rest <<<"$line"
    mv_pad=$(printf "%02d" "$move_id")
    mf="$STAGE/move_$mv_pad/manifest.txt"
    [ -f "$mf" ] || continue
    while read -r mfline; do
        case "$mfline" in '#'*|'') continue ;; esac
        read -r idx fn w h px py missing <<<"$mfline"
        if [ "$missing" = "1" ]; then miss=$((miss+1)); continue; fi
        cel_base=$(basename "$fn" .png)
        cel="$ART_DIR/move_$mv_pad/${cel_base}.CEL"
        if [ ! -f "$cel" ]; then echo "WARN missing CEL on disk: $cel"; fi
        echo "$cel" >> "$MAN"
    done < "$mf"
done < "$STAGE/index.txt"

nframes=$(wc -l < "$MAN")
echo "atlas $NAME: $nframes non-missing frames ($miss missing reuse-prev)"
"$ATLAS" "$ATL_OUT" "@$MAN" || { echo "build_atlas failed"; exit 1; }
echo "wrote $ATL_OUT"
