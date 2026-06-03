# 3DO-OMF2097 port — build wrapper
#
# Primary build driver for Phase 0. Calls into WSL Ubuntu-24.04, sources the
# Trapexit devkit env, then runs `make`. The user never has to switch to WSL
# manually.
#
# IMPORTANT: armlink (ARM SDT 2.51) cannot open files on /mnt/d/ (WSL DrvFS
# 9p mount). Verified 2026-05-18 — armlink reports "File X not found" for
# objects that exist and are readable through every other tool. Workaround:
# mirror the project into a WSL-native dir before invoking `make`, then
# mirror artifacts back. Transparent to the user.
#
# Usage:
#   .\build.ps1            # build .iso
#   .\build.ps1 clean      # clean
#   .\build.ps1 launchme   # build the AIF only

param(
    [Parameter(ValueFromRemainingArguments=$true)]
    [string[]]$MakeArgs
)

$WslDistro     = if ($env:OMF3DO_WSL_DISTRO)  { $env:OMF3DO_WSL_DISTRO }  else { 'Ubuntu-24.04' }
$WslDevkitPath = if ($env:OMF3DO_DEVKIT_PATH) { $env:OMF3DO_DEVKIT_PATH } else { '~/3do-devkit' }
$WslWorkDir    = if ($env:OMF3DO_WORK_DIR)    { $env:OMF3DO_WORK_DIR }    else { '~/.cache/omf2097-3do-build' }

# Translate Windows project dir -> WSL path.
$ProjectDir    = (Get-Item $PSScriptRoot).FullName.Replace('\','/')
$WslProjectDir = (wsl -d $WslDistro -e wslpath -a "$ProjectDir") -join ""
if ($LASTEXITCODE -ne 0) {
    Write-Error "wslpath translation failed (distro: $WslDistro)"
    exit 1
}
$WslProjectDir = $WslProjectDir.Trim()

$MakeArgsJoined = ($MakeArgs -join ' ')

# Build a bash script as a file so we don't have to fight PS heredoc quoting +
# CRLF issues. Write with LF endings explicitly.
# IMPORTANT: PS parses `'foo' + $var + 'bar'` inside @(...) as THREE array
# elements unless wrapped in parens — verified 2026-05-18 by debugging
# inexplicable embedded newlines in this script.
$BashScriptPath = Join-Path $PSScriptRoot ".build-driver.sh"
$BashLines = @(
    '#!/usr/bin/env bash',
    '# NB: no `set -e` — Trapexit activate-env does `which armcc; if [ $? -ne 0 ]; then ...`',
    '#     which trips set -e on first run (armcc absent from initial PATH).',
    ('WORK=$(eval echo "' + $WslWorkDir + '")'),
    ('DEVKIT=$(eval echo "' + $WslDevkitPath + '")'),
    ('PROJ="' + $WslProjectDir + '"'),
    '# WORK_PARENT also holds an openomf-master mirror so the Makefile rule',
    '# build/openomf/%.o : ../openomf-master/src/%.c resolves inside WORK',
    '# (the source tree lives outside port_3do/, so we mirror it as a',
    '# sibling of WORK). Per [[feedback-isolated-project]] we mirror, not',
    '# symlink, to keep the WSL-native build self-contained.',
    'WORK_PARENT=$(dirname "$WORK")',
    'OMF_MIRROR="$WORK_PARENT/openomf-master"',
    'OMF_SRC=$(dirname "$PROJ")/openomf-master',
    'mkdir -p "$WORK" "$OMF_MIRROR" || exit 1',
    'rsync -a --delete \',
    '    --exclude=build \',
    '    --exclude=iso \',
    '    --exclude=takeme/LaunchMe \',
    '    --exclude=.build-driver.sh \',
    '    --exclude=build.log \',
    '    --exclude=exit.log \',
    '    "$PROJ/" "$WORK/" || exit 1',
    'if [ -d "$PROJ/build" ]; then',
    '    rsync -a "$PROJ/build/" "$WORK/build/" || exit 1',
    'fi',
    'if [ -d "$OMF_SRC" ]; then',
    '    rsync -a --delete "$OMF_SRC/" "$OMF_MIRROR/" || exit 1',
    'else',
    '    echo "WARN: openomf-master not found at $OMF_SRC — port_3do/ may fail to link"',
    'fi',
    'source "$DEVKIT/activate-env"',
    'cd "$WORK" || exit 1',
    ('make ' + $MakeArgsJoined),
    'MAKE_RC=$?',
    'rsync -a "$WORK/build/"  "$PROJ/build/"  2>/dev/null || true',
    '# ISO is a fixed-size (padded) CD image: rsync -a compares size+mtime and',
    '# SKIPS the copy-back when size is unchanged and mtime is not newer, so a',
    '# freshly-built ISO silently fails to reach PROJ/ and the emulator loads a',
    '# stale image. --ignore-times forces the transfer every build.',
    'rsync -a --ignore-times "$WORK/iso/" "$PROJ/iso/" 2>/dev/null || true',
    'rsync -a "$WORK/takeme/" "$PROJ/takeme/" 2>/dev/null || true',
    'exit $MAKE_RC'
)

# Write with LF endings.
[IO.File]::WriteAllText($BashScriptPath, ($BashLines -join "`n") + "`n")

$WslScriptPath = (wsl -d $WslDistro -e wslpath -a "$BashScriptPath".Replace('\','/')) -join ""
$WslScriptPath = $WslScriptPath.Trim()

wsl -d $WslDistro -e bash $WslScriptPath
$rc = $LASTEXITCODE
# Keep .build-driver.sh around for debugging until build succeeds at least once.
# Remove-Item -ErrorAction SilentlyContinue $BashScriptPath
exit $rc
