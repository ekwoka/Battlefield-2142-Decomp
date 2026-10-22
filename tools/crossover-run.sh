#!/usr/bin/env bash
# Runs a Windows program inside a CrossOver bottle (default: BF2142).
#   tools/crossover-run.sh build/win32/tests/bf2142_tests.exe
set -euo pipefail
CX="${CROSSOVER_BIN:-/Applications/CrossOver.app/Contents/SharedSupport/CrossOver/bin}"
BOTTLE="${BOTTLE:-BF2142}"
exec "$CX/wine" --bottle "$BOTTLE" --wait-children -- "$@"
