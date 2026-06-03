#!/usr/bin/env bash
#
# regen_assets.sh — ONE-COMMAND regeneration of every OMF:2097-derived artifact
# this port needs (the *_data.h / *_meta.h headers the firmware #includes, plus
# the CELs / atlases / palettes / audio packed into the ISO).
#
# NONE of these are committed to git (they are derivatives of copyrighted game
# data). After a fresh clone, run this once against YOUR own OMF:2097 files to
# reproduce them, then build with ../build.ps1 (or `make`).
#
#   export OMF_DATA=/path/to/your/OMF      # dir with FIGHTR*.AF, ARENA*.BK, ...
#   ./host_tools/regen_assets.sh
#
# Requires: gcc/make/libpng (host tools), the Trapexit 3do-devkit (for 3it), and
# for music ffmpeg built with libopenmpt. Steps are reported individually; the
# headers needed to COMPILE are mandatory, asset steps (music/backgrounds) are
# best-effort and warn if a tool is missing.
set -u
: "${OMF_DATA:?set OMF_DATA to your OMF:2097 data directory (e.g. export OMF_DATA=/path/to/OMF)}"

HT="$(cd "$(dirname "$0")" && pwd)"
DH="$HT/build/dump_har"
DL="$HT/build/dump_lang"
fail=0
step() { echo; echo "==> $*"; }
run()  { if "$@"; then echo "    OK"; else echo "    *** FAILED: $*"; fail=$((fail+1)); fi; }

[ -f "$OMF_DATA/FIGHTR0.AF" ] || { echo "OMF_DATA=$OMF_DATA has no FIGHTR0.AF — wrong dir?"; exit 1; }

step "0. build host tools"
run make -C "$HT"

step "1. HAR movesets + <har>_all_data.h (CELs + per-move frame/step tables)"
run "$HT/run_pipeline_all.sh" jaguar "$OMF_DATA/FIGHTR0.AF" "$OMF_DATA/ARENA0.BK" "$OMF_DATA/SOUNDS.DAT"
run "$HT/run_pipeline_all.sh" thorn  "$OMF_DATA/FIGHTR2.AF" "$OMF_DATA/ARENA2.BK" "$OMF_DATA/SOUNDS.DAT"

step "2. HAR scalar metadata <har>_meta.h (NOT emitted by run_pipeline_all)"
run "$DH" meta "$OMF_DATA/FIGHTR0.AF" "$HT/../src/jaguar_meta.h" jaguar
run "$DH" meta "$OMF_DATA/FIGHTR2.AF" "$HT/../src/thorn_meta.h"  thorn

step "3. Shadow placeholder frames (still #included by hello.c)"
run "$HT/run_pipeline.sh" shadow     "$OMF_DATA/FIGHTR1.AF" "$OMF_DATA/ARENA0.BK" 11
run "$HT/run_pipeline.sh" shadow_dmg "$OMF_DATA/FIGHTR1.AF" "$OMF_DATA/ARENA0.BK" 9

step "4. Pilots, portraits, small font, HAR idles, VS sheet, intro, insults"
run "$HT/run_pipeline_pilots.sh"
run "$HT/run_pipeline_melee_portraits.sh"
run "$HT/run_pipeline_font.sh" small
run "$HT/run_pipeline_har_idles.sh"
run "$HT/run_pipeline_vs_har_sheet.sh"
run "$HT/run_pipeline_intro.sh"
run "$DL" gen-insults "$OMF_DATA/ENGLISH.DAT" "$HT/../src/insults_data.h"

step "5. Per-pilot PRIMARY fight palettes (sentinel atlas + .PAL) — see PALETTE_NOTES.md"
# build_har_atlas_sentinel.sh writes the atlas DIRECTLY to takeme/Art/HAR_<NAME>.ATL
# (per-frame CELs stay in /tmp — they must NOT ship, or the ISO balloons).
run "$HT/build_har_atlas_sentinel.sh" jaguar "$OMF_DATA/FIGHTR0.AF" "$OMF_DATA/ARENA0.BK"
run "$HT/build_har_atlas_sentinel.sh" thorn  "$OMF_DATA/FIGHTR2.AF" "$OMF_DATA/ARENA2.BK"
ART="$HT/../takeme/Art"
mkdir -p "$ART/pluts"
run "$DH" gen-pluts "$OMF_DATA/FIGHTR0.AF" "$OMF_DATA/ARENA0.BK" "$OMF_DATA/ALTPALS.DAT" \
        "$HT/../takeme/Art/HAR_JAGUAR.ATL" "$HT/../takeme/Art/pluts/HAR_JAGUAR"
run "$DH" gen-pluts "$OMF_DATA/FIGHTR2.AF" "$OMF_DATA/ARENA2.BK" "$OMF_DATA/ALTPALS.DAT" \
        "$HT/../takeme/Art/HAR_THORN.ATL" "$HT/../takeme/Art/pluts/HAR_THORN"

step "6. Arena + VS backgrounds (CELs)"
for i in 0 1 2 3 4; do
    run "$HT/run_bg_pipeline.sh" "ARENA${i}" "$OMF_DATA/ARENA${i}.BK"
done
run "$HT/run_vs_bg_pipeline.sh" "$OMF_DATA/VS.BK"

step "7. HUD CELs (health bars, menu frame)"
run "$HT/gen_hpbar_cels.sh"
run "$HT/gen_menu_frame_cels.sh"

step "8. Arena music (PSM -> AIFF; needs ffmpeg + libopenmpt) [best-effort]"
run "$HT/convert_arena_bgms.sh"

echo
echo "============================================================"
if [ "$fail" -eq 0 ]; then
    echo "regen complete — all steps OK."
else
    echo "regen finished with $fail failed step(s) — see output above."
    echo "(music/background steps may fail if ffmpeg/libopenmpt is absent;"
    echo " the *_data.h header steps are the ones required to COMPILE.)"
fi
echo "Next: copy the devkit's System/ boot files into takeme/System/, then build"
echo "with ../build.ps1 (Windows) or 'make' under the devkit activate-env."
echo "============================================================"
exit $fail
