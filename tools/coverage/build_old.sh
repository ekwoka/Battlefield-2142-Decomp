#!/bin/sh
# A release build of an older commit, for before/after measurements.
#
#   tools/coverage/build_old.sh <commit> <dir>
#
# Makes a detached `git worktree` of <commit> at <dir> and builds
# <dir>/build/release/src/Main/bf2142 with no network: FetchContent is
# pointed at the third-party sources this checkout's build/release already
# fetched (ogg, vorbis, zlib, speex). Remove it afterwards with
#   git worktree remove --force <dir>
set -e
[ $# -eq 2 ] || { echo "usage: $0 <commit> <dir>" >&2; exit 2; }
HERE=$(cd "$(dirname "$0")" && pwd)
REPO=$(cd "$HERE/../.." && pwd)
DEPS="$REPO/build/release/_deps"
git -C "$REPO" worktree add --detach "$2" "$1"
cmake -S "$2" -B "$2/build/release" -G Ninja -DCMAKE_BUILD_TYPE=Release \
	-DFETCHCONTENT_FULLY_DISCONNECTED=ON \
	-DFETCHCONTENT_SOURCE_DIR_OGG="$DEPS/ogg-src" \
	-DFETCHCONTENT_SOURCE_DIR_VORBIS="$DEPS/vorbis-src" \
	-DFETCHCONTENT_SOURCE_DIR_ZLIB="$DEPS/zlib-src" \
	-DFETCHCONTENT_SOURCE_DIR_SPEEX="$DEPS/speex-src"
ninja -C "$2/build/release" src/Main/bf2142
