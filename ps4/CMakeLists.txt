# Skylanders: Spyro's Adventure, the PS4 app: the runtime of recomp/ (the PC host's, the same
# files), Crema's interpreter + translated-code loader, the GX renderer on ps4-kit's RADV.
#   ps4/build.sh
cmake_minimum_required(VERSION 3.20)
project(ssa C CXX ASM)
include($ENV{HOME}/ps4-kit/ps4-kit.cmake)
# (the kit's wrap of malloc_usable_size - for apps that call it, Wisp's Skia - leaves an import of the C
# library's malloc_usable_size in this app, which calls it nowhere, and create-fself has no module for
# that symbol: "Failed to build FSELF: missing module for symbol (malloc_usable_size)". Not wrapped here.)
string(REPLACE " --wrap=malloc_usable_size" "" CMAKE_EXE_LINKER_FLAGS "${CMAKE_EXE_LINKER_FLAGS}")

set(SSA_VERSION "00.00" CACHE STRING "Version shown in the log")
# (the Skylanders copy of Crema's source, scripts/setup-crema.sh; CREMA_SRC in the environment chooses
# another; CREMA_DEPS_INCLUDE: the Boost 1.89, glm and fmt 12.1 headers Cemu's code includes)
if(DEFINED ENV{CREMA_SRC})
    set(CREMA_SRC_DEFAULT "$ENV{CREMA_SRC}")
else()
    set(CREMA_SRC_DEFAULT "$ENV{HOME}/ssa-build/crema/src")
endif()
if(DEFINED ENV{CREMA_DEPS_INCLUDE})
    set(CREMA_DEPS_DEFAULT "$ENV{CREMA_DEPS_INCLUDE}")
else()
    set(CREMA_DEPS_DEFAULT "$ENV{HOME}/cemups4-build/sysroot/include")
endif()
set(CREMA_SRC "${CREMA_SRC_DEFAULT}" CACHE PATH "the Skylanders copy of Crema's source (scripts/setup-crema.sh)")
set(CREMA_DEPS_INCLUDE "${CREMA_DEPS_DEFAULT}" CACHE PATH "Boost, glm and fmt headers for Cemu's code")
set(RECOMP ${CMAKE_CURRENT_SOURCE_DIR}/../recomp)

# Crema: the recompiler (its glue and the translated-code loader), the interpreter, PS4 memory mapping
set(CREMA_FILES
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
    util/MemMapper/MemMapperPS4.cpp)
list(TRANSFORM CREMA_FILES PREPEND ${CREMA_SRC}/)

# the runtime (Crema's headers; the same files as the PC host's jittest build)
set(RUNTIME_FILES stubs.cpp ssa_run.cpp mmio.cpp ios.cpp disc.cpp bt.cpp wiimote_crypt.cpp dsp.cpp portal.cpp host.cpp)
list(TRANSFORM RUNTIME_FILES PREPEND ${RECOMP}/)

# the renderer (no Crema headers)
find_program(GLSLANG glslangValidator HINTS $ENV{HOME}/ps4kit-build/host-tools/bin REQUIRED)
set(_shader_headers)
file(MAKE_DIRECTORY ${CMAKE_CURRENT_BINARY_DIR}/shaders)
foreach (shader tev.vert tev.frag copy.vert copy.frag overlay.vert overlay.frag)
    string(REPLACE "." "_" _var ${shader})
    set(_out ${CMAKE_CURRENT_BINARY_DIR}/shaders/${_var}.h)
    add_custom_command(
        OUTPUT ${_out}
        COMMAND ${GLSLANG} -V --quiet --target-env vulkan1.2 --vn spv_${_var} -o ${_out} ${RECOMP}/shaders/${shader}
        DEPENDS ${RECOMP}/shaders/${shader}
        VERBATIM)
    list(APPEND _shader_headers ${_out})
endforeach ()
# (tev.frag with its second output for dual-source blending: the destination alpha constant written
# while blending uses the TEV alpha; gx_vk.cpp picks it when the device has dualSrcBlend)
add_custom_command(
    OUTPUT ${CMAKE_CURRENT_BINARY_DIR}/shaders/tev_frag_dual.h
    COMMAND ${GLSLANG} -V --quiet --target-env vulkan1.2 -DDUAL_SRC=1 --vn spv_tev_frag_dual -o ${CMAKE_CURRENT_BINARY_DIR}/shaders/tev_frag_dual.h ${RECOMP}/shaders/tev.frag
    DEPENDS ${RECOMP}/shaders/tev.frag
    VERBATIM)
list(APPEND _shader_headers ${CMAKE_CURRENT_BINARY_DIR}/shaders/tev_frag_dual.h)

add_library(ssa_crema OBJECT ${CREMA_FILES} ${RUNTIME_FILES} ${CMAKE_CURRENT_SOURCE_DIR}/ps4_main.cpp)
target_compile_features(ssa_crema PRIVATE cxx_std_20)
target_compile_definitions(ssa_crema PRIVATE __ORBIS__ JITTEST SSA_PS4 BOOST_NOWIDE_NO_LIB ENABLE_VULKAN VK_NO_PROTOTYPES
    EMULATOR_HASH=0 EMULATOR_VERSION_MAJOR=0 EMULATOR_VERSION_MINOR=0 EMULATOR_VERSION_PATCH=0
    SSA_PS4_VERSION="v${SSA_VERSION}")
target_include_directories(ssa_crema PRIVATE ${CREMA_SRC} ${CREMA_SRC}/Cafe ${CREMA_SRC}/../dependencies/Vulkan-Headers/include
    ${CREMA_SRC}/gui/interface ${CREMA_SRC}/../dependencies/ZArchive/include ${CREMA_SRC}/../dependencies/imgui ${RECOMP}
    ${PS4_KIT_DIR}/runtime)
target_compile_options(ssa_crema PRIVATE -O2 -g -fno-strict-aliasing -include Common/precompiled.h
    -idirafter ${CREMA_DEPS_INCLUDE} -Wno-everything)

# (the quick menu: overlay, settings, the binding layer, Dear ImGui - Crema's copy, 1.89.5)
set(IMGUI_DIR ${CREMA_SRC}/../dependencies/imgui)
add_library(ssa_gx OBJECT ${RECOMP}/gx.cpp ${RECOMP}/gx_vk.cpp ${CMAKE_CURRENT_SOURCE_DIR}/ps4_input.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/ps4_audio.cpp ${CMAKE_CURRENT_SOURCE_DIR}/ps4_threads.cpp ${_shader_headers}
    ${RECOMP}/overlay.cpp ${RECOMP}/settings.cpp ${RECOMP}/wiimap.cpp
    ${IMGUI_DIR}/imgui.cpp ${IMGUI_DIR}/imgui_draw.cpp ${IMGUI_DIR}/imgui_tables.cpp ${IMGUI_DIR}/imgui_widgets.cpp)
target_compile_features(ssa_gx PRIVATE cxx_std_20)
target_compile_definitions(ssa_gx PRIVATE SSA_PS4)
target_include_directories(ssa_gx PRIVATE ${CMAKE_CURRENT_BINARY_DIR} ${RECOMP} ${CREMA_SRC}/../dependencies/Vulkan-Headers/include
    ${IMGUI_DIR} ${PS4_KIT_DIR}/gpu ${PS4_KIT_DIR}/runtime)
target_compile_options(ssa_gx PRIVATE -O2 -g -Wno-everything)

add_executable(ssa $<TARGET_OBJECTS:ssa_crema> $<TARGET_OBJECTS:ssa_gx>)
target_link_libraries(ssa PRIVATE z -lScePad -lSceUserService -lSceAudioOut -lSceMove)
# (the GDB server's destructor, never called: g_gdbstub stays empty; as build.sh does on the PC)
target_link_options(ssa PRIVATE --defsym=_ZN9GDBServerD1Ev=jittest_noop --defsym=_ZN9GDBServerD2Ev=jittest_noop)
ps4_kit_link_mesa(ssa)
ps4_kit_app(ssa)
