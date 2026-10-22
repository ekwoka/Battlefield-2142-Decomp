#!/usr/bin/env bash
# Swaps rewritten drop-in DLLs into a retail BF2142 install (by default the one
# in the "BF2142" CrossOver bottle), keeping the originals as <name>.retail.
#
#   tools/retail-swap.sh install Memory.dll [more.dll ...]   # from build/win32
#   tools/retail-swap.sh restore Memory.dll                  # put the original back
#   tools/retail-swap.sh status
set -euo pipefail

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
BOTTLE="${BOTTLE:-BF2142}"
GAME_DIR="${RETAIL_GAME_DIR:-$HOME/Library/Application Support/CrossOver/Bottles/$BOTTLE/drive_c/Program Files (x86)/Electronic Arts/Battlefield 2142}"
BUILD_DIR="${BUILD_DIR:-$ROOT/build/win32}"

action="${1:-status}"
shift || true

find_built() {
	local found
	found="$(find "$BUILD_DIR/src" -name "$1" -type f | head -1)"
	[[ -n "$found" ]] || { echo "no built $1 under $BUILD_DIR/src" >&2; exit 1; }
	echo "$found"
}

case "$action" in
	install)
		for dll in "$@"; do
			target="$GAME_DIR/$dll"
			[[ -f "$target.retail" ]] || cp -p "$target" "$target.retail"
			cp "$(find_built "$dll")" "$target"
			echo "installed rewritten $dll"
		done
		;;
	restore)
		for dll in "$@"; do
			target="$GAME_DIR/$dll"
			if [[ -f "$target.retail" ]]; then
				mv "$target.retail" "$target"
				echo "restored retail $dll"
			fi
		done
		;;
	status)
		for backup in "$GAME_DIR"/*.retail; do
			[[ -e "$backup" ]] && echo "rewritten: $(basename "${backup%.retail}")"
		done
		;;
	*)
		echo "usage: $0 install|restore|status [dll...]" >&2
		exit 1
		;;
esac
