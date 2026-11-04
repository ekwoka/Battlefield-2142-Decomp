#!/bin/sh
# Every coverage measurement on one source tree, in order.
#
#   tools/coverage/run_all.sh [--repo <tree>] [--out <dir>] [--before <dir>] [--no-movies-flag <flag>]
#
# --repo   the tree to measure (default: this checkout; its build/release must
#          hold a current bf2142 — `ninja -C <repo>/build/release src/Main/bf2142`)
# --out    where the outputs go (default build/coverage)
# --before an older run's --out, for the before/after tables (rows, runs)
#
# Nothing opens a window: the probe and the level runs use --simulate (and
# --screenshot for the probe's third mode) with --nosound --no-movies; the
# host probe is --host --dedicated without --online, so no service is
# reached.
set -e
HERE=$(cd "$(dirname "$0")" && pwd)
REPO=$(cd "$HERE/../.." && pwd)
OUT="$REPO/build/coverage"
BEFORE=""
NOMOVIES="--no-movies"
while [ $# -gt 0 ]; do
	case "$1" in
	--repo) REPO=$(cd "$2" && pwd); shift 2 ;;
	--out) OUT="$2"; shift 2 ;;
	--before) BEFORE="$2"; shift 2 ;;
	--no-movies-flag) NOMOVIES="$2"; shift 2 ;;
	*) echo "unknown argument $1" >&2; exit 2 ;;
	esac
done
mkdir -p "$OUT"
PY="python3 -I"
R="--repo $REPO --out $OUT"

# game data: one corpus serves every commit
[ -f "$OUT/calls.json" ] || $PY "$HERE/corpus.py" $R
$PY "$HERE/probe.py" $R --no-movies-flag="$NOMOVIES"
if [ -n "$BEFORE" ]; then
	$PY "$HERE/rows.py" $R --before "$BEFORE/rows.json"
	$PY "$HERE/runs.py" $R --no-movies-flag="$NOMOVIES" --before "$BEFORE/runs.json" > "$OUT/runs_table.md"
else
	$PY "$HERE/rows.py" $R
	$PY "$HERE/runs.py" $R --no-movies-flag="$NOMOVIES" > "$OUT/runs_table.md"
fi
$PY "$HERE/cite.py" $R
$PY "$HERE/cite.py" $R --tests --name cite_tests
$PY "$HERE/cite.py" $R --headings --name cite_headings
# without the audits' own reports (their gap lists cite unimplemented code)
$PY "$HERE/cite.py" $R --no-audit-docs --name cite_noaudit
# the decompilation's features are cached in $OUT (they don't depend on the tree)
$PY "$HERE/uncited.py" $R --appendix with
$PY "$HERE/uncited.py" $R --appendix without
$PY "$HERE/uncited.py" $R --cite "$OUT/cite_headings.json" --name uncited_headings
$PY "$HERE/uncited.py" $R --cite "$OUT/cite_noaudit.json" --name uncited_noaudit
$PY "$HERE/hedges.py" $R
