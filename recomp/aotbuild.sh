#!/bin/bash
# A translated code file (cache/aot/<title ID>.aot, loaded by PPCRecompiler.cpp CremaAot*) from the
# C++ a JT_RPX run with JT_AOT_OUT wrote (BackendC.cpp: aot_NNNN.cpp + aot_manifest.cpp):
#   aotbuild.sh <dir> <out.aot> [jobs]
# The same file runs on the PS4 and in these tests: x86-64 for the PS4's Jaguar cores (btver2,
# without SSE4a, which Intel CPUs lack), nothing exported (the loader finds the manifest by the ELF
# entry address), no imports (only RELATIVE relocations), no unwind tables. Objects are kept in
# <dir>/obj and made again only when their .cpp is newer. The generated code comes from the game's
# own files: it stays on this PC and the user's console, never in the repository.
# AOT_PS4=1: objects to link into a game's own app instead (CMake CREMA_AOT_OBJECTS=<dir>/obj-ps4,
# PPCRecompiler.cpp CREMA_AOT_BUILTIN): the PS4 build's target, freestanding (no system headers),
# only compiled; <out.aot> is not written.
# AOT_CFLAGS: more compiler flags, in objects of their own (AOT_OBJ=dir; profile-guided builds: pgo.sh)
set -u
DIR=$1
OUT=$2
JOBS=${3:-16}
TARGET=""
OBJ=${AOT_OBJ:-$DIR/obj}
if [ -n "${AOT_PS4:-}" ]; then
  TARGET="-target x86_64-pc-freebsd12-elf -ffreestanding"
  OBJ=${AOT_OBJ:-$DIR/obj-ps4}
fi
mkdir -p "$OBJ"
echo '#define AOT_VIS __attribute__((visibility("hidden")))' > "$OBJ/aot_build.h"
CXX="clang++-21 $TARGET -std=c++17 -O2 -fPIC -march=btver2 -mno-sse4a -ffp-contract=off -fdenormal-fp-math=preserve-sign -fno-strict-aliasing \
 -fno-exceptions -fno-rtti -fno-asynchronous-unwind-tables -fno-unwind-tables -fno-stack-protector -fcf-protection=none -fvisibility=hidden \
 -fno-builtin-memset -fno-builtin-memcpy -fno-builtin-memmove \
 -include $OBJ/aot_build.h -Wno-everything ${AOT_CFLAGS:-}"
# (-fno-builtin-mem*: with plain guest memory accesses (patch-aot-mem.py) the compiler would make some
# of the game's store loops calls to memset/memcpy, which the file can't import)
# AOT_ORDER=file: the functions it names (one per line, the busiest first: a profile's) placed together
# at the start of the code, in that order (each function its own section; lld's symbol ordering) - the
# hot code in few pages and cache lines (the Jaguar: 32 KiB of L1 instruction cache, small TLBs)
LINKORDER=""
if [ -n "${AOT_ORDER:-}" ]; then
  CXX="$CXX -ffunction-sections"
  LINKORDER="-Wl,--symbol-ordering-file=$AOT_ORDER -Wl,--no-warn-symbol-ordering"
fi
export CXX OBJ
t0=$(date +%s)
ls "$DIR"/aot_*.cpp | xargs -P "$JOBS" -I{} bash -c 'b=$(basename {} .cpp); if [ ! -f "$OBJ/$b.o" ] || [ {} -nt "$OBJ/$b.o" ]; then
  /usr/bin/time -f "%e s %M KB" -o "$OBJ/$b.time" $CXX -c {} -o "$OBJ/$b.o" 2> "$OBJ/$b.err" || echo "FAILED $b"; fi'
echo "compiled in $(($(date +%s) - t0)) s"
for f in "$DIR"/aot_*.cpp; do
  [ -f "$OBJ/$(basename "$f" .cpp).o" ] || { echo "missing object for $f"; exit 1; }
done
if [ -n "${AOT_PS4:-}" ]; then
  # (objects of files no longer written: a smaller game list left them behind)
  for o in "$OBJ"/aot_*.o; do
    [ -f "$DIR/$(basename "$o" .o).cpp" ] || rm -f "$o"
  done
  # the code in a section of its own, which the app's linker script puts after all of Crema's code
  # (toolchain/ps4-love-style.cmake); objects renamed already have no .text left
  for o in "$OBJ"/aot_*.o; do
    if readelf -SW "$o" | grep -q " \.text "; then
      llvm-objcopy-21 --rename-section .text=.crema_aot "$o" || { echo "rename failed: $o"; exit 1; }
    fi
  done
  echo "objects for the app in $OBJ: $(ls "$OBJ"/aot_*.o | wc -l)"
  size -t "$OBJ"/aot_*.o | tail -1
  exit 0
fi
# AOT_LDFLAGS: more link flags (a profile-guided build's -Wl,-z,keep-text-section-prefix: its hot
# functions from all files together, and its cold ones)
clang++-21 -shared -nostdlib -fuse-ld=lld -Wl,-e,aot_manifest -Wl,--no-undefined -Wl,-z,norelro -Wl,--build-id=none -Wl,-z,noexecstack \
  $LINKORDER ${AOT_LDFLAGS:-} -o "$OUT" "$OBJ"/aot_*.o || exit 1
# what the loader takes: RELATIVE relocations only, no constructors or thread-locals, the manifest
# at the entry address
if readelf -rW "$OUT" | grep R_X86_64 | grep -v R_X86_64_RELATIVE | head -3 | grep .; then
  echo "relocations other than RELATIVE"
  exit 1
fi
if readelf -SW "$OUT" | grep -E '\.init_array|\.ctors|\.tdata|\.tbss|\.plt'; then
  echo "constructors, thread-locals or imports"
  exit 1
fi
entry=$(readelf -hW "$OUT" | awk '/Entry point/ {print $4}')
manifest=$(nm "$OUT" | awk '$3 == "aot_manifest" {print "0x" $1}')
if [ -z "$manifest" ] || [ $((entry)) -ne $((manifest)) ]; then
  echo "the entry address $entry is not aot_manifest ($manifest)"
  exit 1
fi
ls -l "$OUT"
size -A "$OUT" | awk '$2 ~ /^[0-9]+$/ && $2 > 65536 {printf "  %-12s %8.1f MiB\n", $1, $2 / 1048576}'
