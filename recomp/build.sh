#!/bin/bash
# Host build of Crema's PowerPC recompiler + interpreter for differential tests (jittest).
#   build.sh [objects|link]
set -u
HERE="$(cd "$(dirname "$0")" && pwd)"
SRC=${JT_SRC:-$HOME/ssa-build/crema/src}
OUT=${JT_OUT:-$HOME/ssa-build/jittest}
mkdir -p $OUT
CXX="clang++-21 -std=c++20 -O1 -g -fno-strict-aliasing -D__ORBIS__ -DJITTEST -DBOOST_NOWIDE_NO_LIB -DENABLE_VULKAN -DVK_NO_PROTOTYPES -DEMULATOR_HASH=0 -DEMULATOR_VERSION_MAJOR=0 -DEMULATOR_VERSION_MINOR=0 -DEMULATOR_VERSION_PATCH=0 -I$SRC -I$SRC/Cafe -I$SRC/../dependencies/Vulkan-Headers/include -I$SRC/gui/interface -I$SRC/../dependencies/ZArchive/include -I$SRC/../dependencies/imgui -include Common/precompiled.h -idirafter $HOME/cemups4-build/sysroot/include -Wno-everything -mavx -mmovbe -msse4.1"
FILES="
Cafe/HW/Espresso/Recompiler/PPCRecompiler.cpp
Cafe/HW/Espresso/Recompiler/PPCRecompilerImlGen.cpp
Cafe/HW/Espresso/Recompiler/PPCRecompilerImlGenFPU.cpp
Cafe/HW/Espresso/Recompiler/PPCRecompilerIntermediate.cpp
Cafe/HW/Espresso/Recompiler/IML/IMLAnalyzer.cpp
Cafe/HW/Espresso/Recompiler/IML/IMLDebug.cpp
Cafe/HW/Espresso/Recompiler/IML/IMLInstruction.cpp
Cafe/HW/Espresso/Recompiler/IML/IMLOptimizer.cpp
Cafe/HW/Espresso/Recompiler/IML/IMLRegisterAllocator.cpp
Cafe/HW/Espresso/Recompiler/IML/IMLRegisterAllocatorRanges.cpp
Cafe/HW/Espresso/Recompiler/IML/IMLSegment.cpp
Cafe/HW/Espresso/Recompiler/BackendX64/BackendX64.cpp
Cafe/HW/Espresso/Recompiler/BackendX64/BackendX64AVX.cpp
Cafe/HW/Espresso/Recompiler/BackendX64/BackendX64BMI.cpp
Cafe/HW/Espresso/Recompiler/BackendX64/BackendX64FPU.cpp
Cafe/HW/Espresso/Recompiler/BackendX64/BackendX64Gen.cpp
Cafe/HW/Espresso/Recompiler/BackendX64/BackendX64GenFPU.cpp
Cafe/HW/Espresso/Recompiler/BackendC/BackendC.cpp
Cafe/HW/Espresso/Interpreter/PPCInterpreterImpl.cpp
Cafe/HW/Espresso/Interpreter/PPCInterpreterFPU.cpp
Cafe/HW/Espresso/Interpreter/PPCInterpreterPS.cpp
Cafe/HW/Espresso/Interpreter/PPCInterpreterOPC.cpp
Cafe/HW/Espresso/Interpreter/PPCInterpreterMain.cpp
Cafe/HW/Espresso/Interpreter/PPCInterpreterHLE.cpp
Common/cpu_features.cpp
"
if [ "${1:-objects}" = objects ]; then
  fail=0
  for f in $FILES; do
    o=$OUT/$(echo $f | tr '/' '_' | sed 's/\.cpp$/.o/')
    if [ ! -f $o ] || [ $SRC/$f -nt $o ]; then
      $CXX -c $SRC/$f -o $o 2> $o.err || { echo "FAILED $f"; head -20 $o.err; fail=1; }
    fi
  done
  exit $fail
fi
if [ "$1" = link ]; then
  $CXX -c "$HERE/stubs.cpp" -o $OUT/stubs.o 2> $OUT/stubs.err || { head -30 $OUT/stubs.err; exit 1; }
  $CXX -c "$HERE/driver.cpp" -o $OUT/driver.o 2> $OUT/driver.err || { head -30 $OUT/driver.err; exit 1; }
  $CXX -c "$HERE/memtrack.cpp" -o $OUT/memtrack.o 2> $OUT/memtrack.err || { head -30 $OUT/memtrack.err; exit 1; }
  $CXX -c "$HERE/ssa_run.cpp" -o $OUT/ssa_run.o 2> $OUT/ssa_run.err || { head -30 $OUT/ssa_run.err; exit 1; }
  $CXX -c "$HERE/mmio.cpp" -o $OUT/mmio.o 2> $OUT/mmio.err || { head -30 $OUT/mmio.err; exit 1; }
  $CXX -c "$HERE/ios.cpp" -o $OUT/ios.o 2> $OUT/ios.err || { head -30 $OUT/ios.err; exit 1; }
  $CXX -c "$HERE/disc.cpp" -o $OUT/disc.o 2> $OUT/disc.err || { head -30 $OUT/disc.err; exit 1; }
  $CXX -c "$HERE/bt.cpp" -o $OUT/bt.o 2> $OUT/bt.err || { head -30 $OUT/bt.err; exit 1; }
  $CXX -c "$HERE/wiimote_crypt.cpp" -o $OUT/wiimote_crypt.o 2> $OUT/wiimote_crypt.err || { head -30 $OUT/wiimote_crypt.err; exit 1; }
  $CXX -c "$HERE/dsp.cpp" -o $OUT/dsp.o 2> $OUT/dsp.err || { head -30 $OUT/dsp.err; exit 1; }
  $CXX -c "$HERE/host.cpp" -o $OUT/host.o 2> $OUT/host.err || { head -30 $OUT/host.err; exit 1; }
  # the GX renderer: shaders to SPIR-V headers, then the decoder and the Vulkan backend (system
  # Vulkan headers and loader; glslang from ~/ssa-build/vkdeps, unpacked from Ubuntu's packages)
  VKD=${VKDEPS:-$HOME/ssa-build/vkdeps/root/usr}
  mkdir -p $OUT/shaders
  for s in tev.vert tev.frag copy.vert copy.frag overlay.vert overlay.frag; do
    n=$(echo $s | tr . _)
    if [ ! -f $OUT/shaders/$n.h ] || [ "$HERE/shaders/$s" -nt $OUT/shaders/$n.h ]; then
      LD_LIBRARY_PATH=$VKD/lib/x86_64-linux-gnu $VKD/bin/glslangValidator -V --target-env vulkan1.2 --vn spv_$n -o $OUT/shaders/$n.h "$HERE/shaders/$s" > $OUT/shaders/$n.err 2>&1 || { cat $OUT/shaders/$n.err; exit 1; }
    fi
  done
  # (tev.frag with its second output for dual-source blending, gx_vk.cpp's choice on devices that have it)
  if [ ! -f $OUT/shaders/tev_frag_dual.h ] || [ "$HERE/shaders/tev.frag" -nt $OUT/shaders/tev_frag_dual.h ]; then
    LD_LIBRARY_PATH=$VKD/lib/x86_64-linux-gnu $VKD/bin/glslangValidator -V --target-env vulkan1.2 -DDUAL_SRC=1 --vn spv_tev_frag_dual -o $OUT/shaders/tev_frag_dual.h "$HERE/shaders/tev.frag" > $OUT/shaders/tev_frag_dual.err 2>&1 || { cat $OUT/shaders/tev_frag_dual.err; exit 1; }
  fi
  IMGUI=$SRC/../dependencies/imgui
  VKCXX="clang++-21 -std=c++20 -O2 -g -I$VKD/include -I$OUT -I$IMGUI"
  # the quick menu (overlay, settings, the binding layer) and Dear ImGui (Crema's copy, 1.89.5)
  for f in overlay settings wiimap; do
    $VKCXX -c "$HERE/$f.cpp" -o $OUT/$f.o 2> $OUT/$f.err || { head -30 $OUT/$f.err; exit 1; }
  done
  for f in imgui imgui_draw imgui_tables imgui_widgets; do
    [ -f $OUT/$f.o ] || $VKCXX -c "$IMGUI/$f.cpp" -o $OUT/$f.o 2> $OUT/$f.err || { head -30 $OUT/$f.err; exit 1; }
  done
  MENU_OBJS="$OUT/overlay.o $OUT/settings.o $OUT/wiimap.o $OUT/imgui.o $OUT/imgui_draw.o $OUT/imgui_tables.o $OUT/imgui_widgets.o"
  $VKCXX -c "$HERE/gx.cpp" -o $OUT/gx.o 2> $OUT/gx.err || { head -30 $OUT/gx.err; exit 1; }
  $VKCXX -c "$HERE/gx_vk.cpp" -o $OUT/gx_vk.o 2> $OUT/gx_vk.err || { head -30 $OUT/gx_vk.err; exit 1; }
  # gx_replay: a GX recording (SSA_GXREC) drawn again (renderer work in seconds)
  mkdir -p $OUT/replay
  $VKCXX -c "$HERE/gx_replay.cpp" -o $OUT/replay/gx_replay.o 2> $OUT/replay/gx_replay.err || { head -30 $OUT/replay/gx_replay.err; exit 1; }
  clang++-21 -o $OUT/gx_replay $OUT/replay/gx_replay.o $OUT/gx.o $OUT/gx_vk.o $MENU_OBJS -lz -lpthread /usr/lib/x86_64-linux-gnu/libvulkan.so.1 || exit 1
  $CXX -c "$HERE/portal.cpp" -o $OUT/portal.o 2> $OUT/portal.err || { head -30 $OUT/portal.err; exit 1; }
  clang++-21 -o $OUT/jittest $OUT/*.o -lpthread -ldl -lz /usr/lib/x86_64-linux-gnu/libvulkan.so.1 -rdynamic -Wl,--defsym=_ZN9GDBServerD1Ev=jittest_noop -Wl,--defsym=_ZN9GDBServerD2Ev=jittest_noop 2>&1 | grep -o "undefined reference to \`[^']*'" | sort -u | head -80
  ls -l $OUT/jittest 2>/dev/null
fi
