#!/bin/bash
# The translated code from your own disc, on the PC (WSL 2): the .aot file the app loads.
#   scripts/build-aot.sh <extracted disc dir> <out.aot> [pgo]
# <extracted disc dir>: tools/extract-disc.sh's output (sys/main.dol in it, SHA-1
# 161858104eed2fd9e9f76cd692e99ebc7c7fdca8). Needs the PC harness (recomp/build.sh, from
# scripts/setup-crema.sh's tree) in ~/ssa-build/jittest, clang/lld/LLVM 21. Works in ~/ssa-build/p1.
# "pgo": a profile-guided build (recomp/pgo.sh: the game runs 400 s on the PC first, about 15% faster
# code; needs a Vulkan driver on the PC); without it, plain -O2.
# The translation is the one in the releases: 26,214 functions (analysis/entries-aot.txt), registers
# kept across calls and the save/restore helpers expanded (patch-aot-calls.py, patch-aot-expand.py),
# hardware accesses where they happen (patch-aot-hw.py select), mfcr shortened (patch-aot-mfcr.py).
# What it makes holds the game's code: keep it to your own consoles.
set -euo pipefail
HERE="$(cd "$(dirname "$0")/.." && pwd)"
DISC="$(cd "${1:?extracted disc dir}" && pwd)"
OUT="$(realpath -m "${2:?out .aot}")"
MODE="${3:-}"
DOL="$DISC/sys/main.dol"
JT=~/ssa-build/jittest/jittest
[ -f "$DOL" ] || { echo "no $DOL"; exit 1; }
[ -x "$JT" ] || { echo "no $JT: build the PC harness first (recomp/build.sh objects && recomp/build.sh link)"; exit 1; }
if [ "$(sha1sum "$DOL" | cut -c1-40)" != 161858104eed2fd9e9f76cd692e99ebc7c7fdca8 ]; then
    echo "$DOL isn't SSPE52 revision 1's main.dol: the translation is for that one only"
    exit 1
fi
P1=~/ssa-build/p1
mkdir -p $P1 && cd $P1
python3 "$HERE/tools/dol2bin.py" "$DOL" code.bin > /dev/null
cp "$HERE/analysis/entries-aot.txt" entries-aot.txt
rm -rf aot && mkdir aot
echo "translating..."
JT_RPX=code.bin JT_FUNCS=entries-aot.txt JT_AOT_OUT=aot JT_AOT_KEEP=8034f280-8034f3b0 JT_AOT_EXPAND=8034f280-8034f3b0 \
    "$JT" > aot.gen.log 2>&1 || { tail -20 aot.gen.log; exit 1; }
grep -a "^AOT:" aot.gen.log | tail -1
python3 "$HERE/recomp/patch-aot-hw.py" select aot "$DOL" "$HERE/analysis/hw-trapped.txt" | tail -1
python3 "$HERE/recomp/patch-aot-mfcr.py" aot aot
if [ "$MODE" = pgo ]; then
    SSA_PGO_DISC="$DISC" bash "$HERE/recomp/pgo.sh" aot "$OUT" 400
else
    bash "$HERE/recomp/aotbuild.sh" aot "$OUT" "$(nproc)" | tail -4
fi
ls -l "$OUT"
