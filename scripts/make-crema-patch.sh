#!/bin/bash
# patches/crema-ssa.patch again, after the Crema copy changed (a new recomp/patch-*.py applied to it):
# upstream Cemu's src/ at patches/BASE.txt's commit -> the copy's src/, as a git diff (new, deleted and
# binary files too), then checked by applying it to a fresh archive of the base.
#   scripts/make-crema-patch.sh [copy]        (default ~/ssa-build/crema; CEMU: a clone of upstream
#                                              Cemu that has the base commit, default ~/cemu-upstream)
set -euo pipefail
HERE="$(cd "$(dirname "$0")/.." && pwd)"
COPY="${1:-$HOME/ssa-build/crema}"
CEMU="${CEMU:-$HOME/cemu-upstream}"
BASE=$(grep -o -m1 "[0-9a-f]\{40\}" "$HERE/patches/BASE.txt")
[ -d "$CEMU/.git" ] || git clone https://github.com/cemu-project/Cemu "$CEMU"
W=$(mktemp -d)
trap 'rm -rf "$W"' EXIT
mkdir -p "$W/repo" "$W/check"
cd "$W/repo" && git init -q
git -C "$CEMU" archive "$BASE" src | tar -x -C "$W/repo"
git add -A src && git -c user.name=base -c user.email=base@localhost commit -q -m base
rm -rf src && cp -a "$COPY/src" src
git add -A src
git diff --cached --binary --full-index > "$HERE/patches/crema-ssa.patch"
echo "$(grep -c '^diff --git' "$HERE/patches/crema-ssa.patch") files changed: $HERE/patches/crema-ssa.patch"
cd "$W/check" && git init -q
git -C "$CEMU" archive "$BASE" src | tar -x -C "$W/check"
git apply --whitespace=nowarn "$HERE/patches/crema-ssa.patch"
if diff -r "$W/check/src" "$COPY/src" > /dev/null; then echo "checked: the patch makes the copy exactly"; else echo "CHECK FAILED"; exit 1; fi
