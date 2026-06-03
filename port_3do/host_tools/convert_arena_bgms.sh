#!/usr/bin/env bash
: "${OMF_DATA:?set OMF_DATA to your OMF:2097 data directory (e.g. export OMF_DATA=/path/to/OMF)}"
# convert_arena_bgms.sh -- one-shot PSM -> AIFF converter for the 5 OMF arena BGMs.
# Uses ffmpeg+libopenmpt to decode the proprietary PSM tracker format, then
# encodes 44100 Hz stereo s16 (fixedstereosample.dsp's expected input on 3DO).
set -e

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
PROJ="$(cd "$SCRIPT_DIR/.." && pwd)"
SRC_DIR="${OMF_DATA}"
DST_DIR="$PROJ/takeme/Music"

mkdir -p "$DST_DIR"

for i in 0 1 2 3 4; do
    src="$SRC_DIR/ARENA${i}.PSM"
    dst="$DST_DIR/ARENA${i}.AIFF"
    if [ ! -f "$src" ]; then
        echo "WARN: $src missing, skipping"
        continue
    fi
    echo "[convert] $src -> $dst"
    ffmpeg -y -loglevel error -i "$src" -ar 44100 -ac 2 -sample_fmt s16 "$dst"
done

ls -la "$DST_DIR"/ARENA*.AIFF
