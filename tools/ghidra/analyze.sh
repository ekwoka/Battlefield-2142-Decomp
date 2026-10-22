#!/usr/bin/env bash
# Imports the BF2142 binaries into a shared Ghidra project, runs auto-analysis,
# then applies our symbols and exports inventories to build/analysis/<module>/.
#
# Usage:
#   tools/ghidra/analyze.sh                 # all first-party modules
#   tools/ghidra/analyze.sh BF2142.exe      # specific modules
#   REANALYZE=1 tools/ghidra/analyze.sh     # re-import even if already done
#   DECOMPILE=1 tools/ghidra/analyze.sh     # also dump decompiled C to build/analysis/<module>/decomp/
#
# After a first DECOMPILE=1 run, generate build/analysis/<module>/conventions.csv
# with tools/decomp/find_register_args.py and run again: it marks member
# functions __thiscall so the decompiler shows their `this` argument.
#
# Environment:
#   GAME_DIR      game install (default: source/Battlefield 2142)
#   GHIDRA_HOME   Ghidra install (default: Homebrew's ghidra)
#   JAVA_HOME     JDK 21 (default: Homebrew's openjdk@21)
set -euo pipefail

ROOT="$(cd "$(dirname "$0")/../.." && pwd)"
GAME_DIR="${GAME_DIR:-$ROOT/source/Battlefield 2142}"
GHIDRA_HOME="${GHIDRA_HOME:-$(brew --prefix ghidra 2>/dev/null)/libexec}"
HEADLESS="$GHIDRA_HOME/support/analyzeHeadless"
# Ghidra 12 needs JDK 21; macOS's /usr/bin/java stub can't locate it on its own.
export JAVA_HOME="${JAVA_HOME:-$(brew --prefix openjdk@21 2>/dev/null)/libexec/openjdk.jdk/Contents/Home}"
PROJECT_DIR="$ROOT/build/ghidra"
PROJECT_NAME="bf2142"
OUT_ROOT="$ROOT/build/analysis"
SCRIPTS="$ROOT/tools/ghidra/scripts"

# First-party DICE modules built from the Patch_1_51 branch, largest first.
DEFAULT_MODULES=(
	BF2142.exe
	RendDX9_ori.dll
	AIDLL.dll
	SwiffPlayer.dll
	BF2142Audio.dll
	TextureAtlasBuilder.dll
	BF2VoipServer.dll
	Memory.dll
)

if [[ ! -x "$HEADLESS" ]]; then
	echo "analyzeHeadless not found at $HEADLESS (brew install ghidra, or set GHIDRA_HOME)" >&2
	exit 1
fi

modules=("$@")
[[ ${#modules[@]} -eq 0 ]] && modules=("${DEFAULT_MODULES[@]}")

mkdir -p "$PROJECT_DIR" "$OUT_ROOT"

for module in "${modules[@]}"; do
	binary="$GAME_DIR/$module"
	out="$OUT_ROOT/$module"
	marker="$out/.imported"
	mkdir -p "$out"

	if [[ ! -f "$binary" ]]; then
		echo "missing $binary" >&2
		exit 1
	fi

	post_scripts=(
		-postScript CreateStaticInitializers.java
		-postScript ApplyCallingConventions.java "$out/conventions.csv"
		-postScript FindVtables.java "$out"
		-postScript ApplyKnownSymbols.java "$ROOT/symbols/$module.csv"
		-postScript TagSourceFiles.java "$out"
		-postScript ExportFunctions.java "$out"
	)
	if [[ -n "${DECOMPILE:-}" ]]; then
		post_scripts+=(-postScript ExportDecompiled.java "$out")
	fi

	if [[ -f "$marker" && -z "${REANALYZE:-}" ]]; then
		echo "==> $module: already imported, refreshing symbols and exports"
		"$HEADLESS" "$PROJECT_DIR" "$PROJECT_NAME" \
			-process "$module" -noanalysis \
			-scriptPath "$SCRIPTS" "${post_scripts[@]}" \
			-log "$out/ghidra.log"
	else
		echo "==> $module: importing and analysing (this can take a while)"
		"$HEADLESS" "$PROJECT_DIR" "$PROJECT_NAME" \
			-import "$binary" -overwrite \
			-scriptPath "$SCRIPTS" -preScript ConfigureAnalysis.java "${post_scripts[@]}" \
			-log "$out/ghidra.log"
		touch "$marker"
	fi
done

echo "Done. Open the project with: ghidraRun, then File > Open Project > $PROJECT_DIR/$PROJECT_NAME.gpr"
