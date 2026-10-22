#!/usr/bin/env bash
# Smoke-tests the retail game in a CrossOver bottle: launches it windowed,
# waits, then reports whether it is still alive and how much memory it uses.
#   tools/retail-smoke.sh [seconds] [extra game args...]
set -uo pipefail
BOTTLE="${BOTTLE:-BF2142}"
CX="${CROSSOVER_BIN:-/Applications/CrossOver.app/Contents/SharedSupport/CrossOver/bin}"
GAME_DIR="${RETAIL_GAME_DIR:-$HOME/Library/Application Support/CrossOver/Bottles/$BOTTLE/drive_c/Program Files (x86)/Electronic Arts/Battlefield 2142}"
SECONDS_TO_WAIT="${1:-45}"
shift || true
LOG="${SMOKE_LOG:-/tmp/bf2142-smoke.log}"

cd "$GAME_DIR"
"$CX/wine" --bottle "$BOTTLE" --cx-app BF2142.exe -- BF2142.exe +menu 1 +fullscreen 0 +szx 1024 +szy 768 +modPath mods/bf2142 "$@" >"$LOG" 2>&1 &
sleep "$SECONDS_TO_WAIT"

pid="$(pgrep -f 'Battlefield 2142\\BF2142.exe' | head -1)"
if [[ -n "$pid" ]]; then
	ps -o rss=,%cpu= -p "$pid" | awk '{printf "ALIVE rss=%d MB cpu=%s%%\n", $1/1024, $2}'
	kill "$pid"
	sleep 2
	"$CX/wineserver" --bottle "$BOTTLE" -k 2>/dev/null || pkill -f wineserver || true
	exit 0
fi
echo "DEAD (see $LOG)"
grep -v -E ':fixme:|^msync' "$LOG" | tail -20
exit 1
