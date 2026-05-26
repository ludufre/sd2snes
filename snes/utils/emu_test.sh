#!/usr/bin/env bash
# Build → launch bsnes-plus → screenshot. Outputs the PNG path on stdout.
#
# Usage:
#   ./snes/utils/emu_test.sh                     # build then capture
#   ./snes/utils/emu_test.sh skip-build          # just relaunch + capture
set -euo pipefail

BSNES="/Users/ludufre/Developer/bsnes-plus/bsnes+.app/Contents/MacOS/bsnes"
ROM_OUT="/tmp/sd2snes-test/menu.sfc"
SHOT="/tmp/bsnes-shot.png"
REPO="$(cd "$(dirname "$0")/../.." && pwd)"

cd "$REPO"

if [[ "${1:-}" != "skip-build" ]]; then
    ./build.sh >/dev/null 2>&1
fi
mkdir -p "$(dirname "$ROM_OUT")"
cp release/v1.11.2/sd2snes/menu.bin "$ROM_OUT"

# Kill any running bsnes, then launch fresh.
pkill -9 bsnes 2>/dev/null || true
sleep 0.5
"$BSNES" "$ROM_OUT" >/dev/null 2>&1 &
BSNES_PID=$!

# Wait for the window to appear and the menu to reach emu_mode (~1s timeout
# inside wait_mcu_ready plus a couple of frames of NMI rendering).
sleep 3

# Find the bsnes window geometry via AppleScript.
GEOM=$(osascript -e 'tell application "System Events"
    set p to first process whose name is "bsnes"
    tell p to set {x, y} to position of window 1
    tell p to set {w, h} to size of window 1
    return (x as text) & "," & (y as text) & "," & (w as text) & "," & (h as text)
end tell' 2>/dev/null)

if [[ -z "$GEOM" ]]; then
    echo "ERROR: could not locate bsnes window" >&2
    exit 1
fi

screencapture -x -R "$GEOM" "$SHOT" 2>/dev/null
echo "$SHOT"
