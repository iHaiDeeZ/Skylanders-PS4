#!/bin/bash
# A profile-guided build of translated code (WSL, the PC host built):
#   pgo.sh <aot dir> <out.aot> [run seconds]          (from ~/ssa-build/p1, dir relative to it)
# 1. the C++ built with LLVM's counters (aotbuild.sh AOT_CFLAGS=-fprofile-generate, objects in
#    <dir>/obj-gen): <dir>-gen.aot - no profile runtime in it (it imports nothing)
# 2. the scripted PC run with it (the same as the speed tests': NAND copy, Spyro on the portal, A
#    pressed through the menus into the village), its counters written at the end (ssa_run.cpp
#    SSA_AOT_DUMP=file,from,size: from the entry function's address)
# 3. pgo-profraw.py: a .profraw from the file's records and names and the dumped counters;
#    llvm-profdata; the OS idle loop's counts zeroed (pgo-zero.py: the PC's game thread idles ~80%,
#    counted ~3e14 there; left in, nothing else would look hot)
# 4. the C++ built with the profile (objects in <dir>/obj-use): -fprofile-use and -mllvm -pgso=false
#    (code the run didn't reach compiled for speed as before, not for size); AOT_LDFLAGS as given
#    (e.g. -Wl,-z,keep-text-section-prefix)
# The run covers boot, menus and the village: the profile is that. The generated code stays local.
# SSA_PGO_DISC: your extracted disc (sys/main.dol in it; default the repository's disc/DATA); the
# NAND: the repository's nand/ when there is one, else a new one (tools/make-nand.py). The run needs
# a Vulkan driver on the PC (Mesa's lavapipe: Ubuntu's mesa-vulkan-drivers).
set -euo pipefail
DIR=$1; OUT=$2; SECS=${3:-400}
X="$(cd "$(dirname "$0")" && pwd)"
DISC="${SSA_PGO_DISC:-$X/../disc/DATA}"
[ -f "$DISC/sys/main.dol" ] || { echo "no $DISC/sys/main.dol: SSA_PGO_DISC=<your extracted disc>"; exit 1; }
P1=~/ssa-build/p1
mkdir -p $P1
cd $P1
base=$(basename "$DIR")
GEN=$P1/$base-gen.aot
echo "== 1. instrumented build"
AOT_OBJ=$P1/$DIR/obj-gen AOT_CFLAGS="-fprofile-generate -mllvm -disable-vp=true" bash "$X/aotbuild.sh" "$DIR" "$GEN" 16 | tail -3
read caddr csize < <(python3 "$X/pgo-profraw.py" "$GEN" --where)
eaddr=$(nm --defined-only "$GEN" | awk '$3 == "aot_80006310" {print "0x" $1}') # (no early exit: pipefail)
from=$(( caddr - eaddr ))
echo "== 2. the run ($SECS s), counters $csize bytes at entry + $from"
tag=pgo-$base
rm -rf $P1/nand-$tag $P1/frames-$tag $P1/figures-$tag
if [ -d "$X/../nand" ]; then cp -r "$X/../nand" $P1/nand-$tag; else python3 "$X/../tools/make-nand.py" $P1/nand-$tag > /dev/null; fi
mkdir -p $P1/frames-$tag $P1/figures-$tag
cp $P1/figures/spyro.sky $P1/figures-$tag/ 2>/dev/null || true
P=70:A,75:A,85:A,100:A,120:A,140:A,160:A,180:A
for t in $(seq 200 10 $((SECS - 20))); do P=$P,$t:A; done
SSA_RUN=1 SSA_NAND=$P1/nand-$tag SSA_DOL="$DISC/sys/main.dol" SSA_AOT=$GEN \
  SSA_DISC="$DISC" SSA_SECONDS=$SECS SSA_GX_LOG=1 SSA_PRESS=$P SSA_PORTAL=figures-$tag/spyro.sky=16 \
  SSA_GX_THREAD=1 SSA_GX_WORKERS=3 SSA_AOT_GP=1 SSA_AOT_DUMP=$P1/$tag.cnts,$from,$csize \
  ~/ssa-build/jittest/jittest > run-$tag.log 2>&1 || true
grep -a -E "stopped after|SSA_AOT_DUMP|^ssa: signal" run-$tag.log | head -3
echo "== 3. the profile"
python3 "$X/pgo-profraw.py" "$GEN" $P1/$tag.cnts $P1/$tag.profraw
llvm-profdata-21 merge --text -o $P1/$tag.proftext $P1/$tag.profraw
python3 "$X/pgo-zero.py" $P1/$tag.proftext aot_80387480
llvm-profdata-21 merge -o $P1/$tag.profdata $P1/$tag.proftext
llvm-profdata-21 show $P1/$tag.profdata | grep -E "Total functions|Maximum function count|Total count"
echo "== 4. the profile-guided build"
AOT_OBJ=$P1/$DIR/obj-use AOT_CFLAGS="-fprofile-use=$P1/$tag.profdata -mllvm -pgso=false" bash "$X/aotbuild.sh" "$DIR" "$OUT" 16 | tail -4
for e in $P1/$DIR/obj-use/*.err; do [ -s "$e" ] && { echo "errors in $e"; head -3 "$e"; } || true; done
