#!/usr/bin/env bash
# Checks that the enhanced mode leaves the simulation alone: runs the same
# headless bot round twice without --enhanced (the round must repeat),
# then once with it, and compares the state fingerprints. The enhanced
# gameplay additions (enhanced.botRecovery) change the simulation by
# design and are turned off for the comparison.
#   tools/parity-check.sh [seconds] [extra game args...]
set -uo pipefail
B="${BF2142_BIN:-build/release/src/Main/bf2142}"
G="${BF2142_GAME:-source/Battlefield 2142}"
SECS="${1:-60}"
shift || true
EXTRA=("$@")

run() {
	"$B" --game "$G" +level Cerbere_Landing +gameMode gpm_sp +mapSize 16 --bots 15 --simulate "$SECS" \
		"$@" ${EXTRA[@]+"${EXTRA[@]}"} 2>&1 | awk '/state fingerprint/ {print $3}'
}

off1="$(run)"
off2="$(run)"
on="$(run --enhanced --console "enhanced.botRecovery 0" --console "enhanced.botMovement 0" --console "enhanced.botTitan 0" --console "enhanced.cornerSteer 0")"
echo "off: $off1 / $off2, on: $on"
if [[ -z "$off1" ]]; then
	echo "FAIL: no fingerprint printed"; exit 1
elif [[ "$off1" != "$off2" ]]; then
	echo "FAIL: the round doesn't repeat with the switch off, so the check can't tell"; exit 1
elif [[ "$off1" != "$on" ]]; then
	echo "FAIL: --enhanced changed the simulation"; exit 1
fi
echo "PASS"
