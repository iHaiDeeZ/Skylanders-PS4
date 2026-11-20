#!/bin/bash
# The Skylanders copy of Crema's source: the PowerPC recompiler, its C++ backend (BackendC) and the
# translated-code loader, the interpreter - upstream Cemu at patches/BASE.txt's commit, the three
# submodules the builds use, and patches/crema-ssa.patch (Crema's changes and this port's patch
# scripts, already applied):
#   scripts/setup-crema.sh [dir]        (default ~/ssa-build/crema: where recomp/build.sh and
#                                        ps4/build.sh look; JT_SRC / CREMA_SRC=dir/src choose another)
set -euo pipefail
HERE="$(cd "$(dirname "$0")/.." && pwd)"
DIR="${1:-$HOME/ssa-build/crema}"
BASE=$(grep -o -m1 "[0-9a-f]\{40\}" "$HERE/patches/BASE.txt")
if [ -e "$DIR" ]; then
    echo "$DIR is there already: move it away first"
    exit 1
fi
git clone https://github.com/cemu-project/Cemu "$DIR"
cd "$DIR"
git checkout -q "$BASE"
git submodule update --init dependencies/Vulkan-Headers dependencies/ZArchive dependencies/imgui
git apply --whitespace=nowarn "$HERE/patches/crema-ssa.patch"
echo "the Skylanders copy of Crema is in $DIR (Cemu $BASE + patches/crema-ssa.patch)"
