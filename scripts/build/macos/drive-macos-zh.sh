#!/usr/bin/env bash
# GeneralsX @build Claude 27/07/2026 - drive the deployed macOS build with synthetic input.
#
# Mac-side counterpart of scripts/build/windows/drive-run-win64.ps1, and it exists for the same
# reason: there is NO command-line flag in either tree that hosts or joins a network game (verified
# by unbounded search over both CommandLine.cpp parse tables), so a LAN test has to be driven
# through the shell UI.
#
# Coordinates are CLIENT-relative and translated at runtime. The window does NOT land in the same
# place twice - measured at (61,30) on one launch and (448,80) on the next - so hardcoded desktop
# coordinates silently miss, the menu never advances, and the only symptom is an absence of [LAN]
# traces. Same failure that cost an hour on the Windows side.
#
# Requires: cliclick (brew install cliclick) and Accessibility permission for the calling terminal.
#
# Usage:
#   ./scripts/build/macos/drive-macos-zh.sh --clicks "1287,260;1287,260" --tag lobby
#   ./scripts/build/macos/drive-macos-zh.sh --shot-only --tag check
#
# Useful client coordinates at 1600x900 (the menu column is at client x=1287):
#   main menu   SOLO PLAY 200  MULTIPLAYER 260  LOAD 320  OPTIONS 380
#   mp submenu  ONLINE    200  NETWORK     260  BACK 320
#   LAN lobby   CREATE GAME (280,786)  JOIN GAME (625,786)  games list row 1 (700,215)
#   game opts   PLAY GAME / ACCEPT (327,799)  BACK (1253,799)

set -uo pipefail

CLICKS=""
TAG="drive"
BETWEEN=6
DWELL=0.9
OUTDIR="${GX_DRIVE_OUTDIR:-/tmp/gx-drive}"
SHOT_ONLY=0

while [[ $# -gt 0 ]]; do
    case "$1" in
        --clicks)    CLICKS="$2"; shift 2 ;;
        --tag)       TAG="$2"; shift 2 ;;
        --between)   BETWEEN="$2"; shift 2 ;;
        --outdir)    OUTDIR="$2"; shift 2 ;;
        --shot-only) SHOT_ONLY=1; shift ;;
        *) echo "unknown arg: $1" >&2; exit 2 ;;
    esac
done

command -v cliclick >/dev/null || { echo "ERROR: cliclick not installed (brew install cliclick)" >&2; exit 1; }
mkdir -p "$OUTDIR"

if ! pgrep -f GeneralsXZH >/dev/null; then
    echo "ERROR: GeneralsXZH is not running" >&2
    exit 1
fi

# Raise the game. Without this the clicks land on whatever is in front.
osascript -e 'tell application "System Events" to set frontmost of (first process whose name is "GeneralsXZH") to true' 2>/dev/null
sleep 2

# Window geometry, queried fresh every run because it moves between launches.
GEOM=$(osascript -e 'tell application "System Events" to tell process "GeneralsXZH" to get {position, size} of window 1' 2>/dev/null)
[[ -z "$GEOM" ]] && { echo "ERROR: could not read window geometry" >&2; exit 1; }

WX=$(echo "$GEOM" | cut -d, -f1 | tr -d ' ')
WY=$(echo "$GEOM" | cut -d, -f2 | tr -d ' ')
WW=$(echo "$GEOM" | cut -d, -f3 | tr -d ' ')
WH=$(echo "$GEOM" | cut -d, -f4 | tr -d ' ')

# The client area is the window minus the title bar. Derive the bar height rather than assuming 32:
# it differs with the window style and would silently shift every y coordinate.
CLIENT_H=900
TITLE_H=$(( WH - CLIENT_H ))
(( TITLE_H < 0 )) && TITLE_H=0
OX=$WX
OY=$(( WY + TITLE_H ))

echo "[drive] window ${WW}x${WH} at (${WX},${WY}); title bar ${TITLE_H}px; client origin (${OX},${OY})"

shot() {
    local name="$1"
    screencapture -x "${OUTDIR}/${TAG}-${name}.png"
    # Crop to the client area so the image is directly in client coordinates.
    python3 - "$OUTDIR/${TAG}-${name}.png" "$OX" "$OY" <<'PY' 2>/dev/null || true
import sys
from PIL import Image
p, ox, oy = sys.argv[1], int(sys.argv[2]), int(sys.argv[3])
im = Image.open(p)
im.crop((ox, oy, ox + 1600, oy + 900)).save(p.replace('.png', '-client.png'))
PY
}

shot "step0"
[[ $SHOT_ONLY -eq 1 ]] && { echo "[drive] shot only, done"; exit 0; }

i=0
IFS=';' read -ra STEPS <<< "$CLICKS"
for step in "${STEPS[@]}"; do
    step="$(echo "$step" | tr -d ' ')"
    [[ -z "$step" ]] && continue
    i=$((i+1))

    hover_only=0
    if [[ "$step" == h:* ]]; then hover_only=1; step="${step#h:}"; fi

    cx="${step%,*}"; cy="${step#*,}"
    sx=$(( OX + cx )); sy=$(( OY + cy ))

    # Approach from an offset so a real move event is generated, settle on the target, then DWELL
    # before pressing. The shell resolves the hovered control once per frame at 30 Hz logic, so a
    # press issued in the same instant as the move is attributed to the PREVIOUSLY hovered control.
    # On Windows this exact mistake made a click on NETWORK open ONLINE.
    cliclick "m:${sx},$(( sy - 60 ))"
    sleep 0.25
    cliclick "m:${sx},${sy}"
    sleep "$DWELL"

    if [[ $hover_only -eq 1 ]]; then
        echo "[drive] hover$i client(${cx},${cy}) screen(${sx},${sy})"
    else
        cliclick "c:${sx},${sy}"
        echo "[drive] click$i client(${cx},${cy}) screen(${sx},${sy})"
    fi

    sleep "$BETWEEN"
    shot "step${i}"

    if ! pgrep -f GeneralsXZH >/dev/null; then
        echo "[drive] process DIED after step $i" >&2
        exit 1
    fi
done

echo "[drive] done; artifacts in ${OUTDIR} (files ending -client.png are in client coordinates)"
