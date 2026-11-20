#!/usr/bin/env bash
# The PS4 app: build and package as XX/out/Skylanders-SSA-vXX.YY.pkg (title SKYL00001). Run in WSL.
#   SSA_VERSION=00.01 ps4/build.sh [clean]
# Then, from Windows (Git Bash): ~/ps4-kit/scripts/upload.sh out/Skylanders-SSA-vXX.YY.pkg <console-ip>
# The game's files are not in the package: they go to the console's /data/SSA (disc/, ssa8.aot,
# nand/; see ps4_main.cpp).
set -euo pipefail
HERE="$(cd "$(dirname "$0")" && pwd)"
source "$HOME/ps4-kit/toolchain/env.sh"
VERSION="${SSA_VERSION:-00.00}"
B="$HOME/ssa-build/ps4"
[ "${1:-}" = clean ] && rm -rf "$B"
mkdir -p "$B" "$PSB/logs"
export PATH="$PSB/host-tools/bin:$PATH"
need cmake ninja glslangValidator
[ -f "$PS4_MESA_BUILD/src/amd/vulkan/libvulkan_radeon.a" ] || bash "$PS4_KIT/gpu/build-mesa.sh"

ps4_cmake -S "$HERE" -B "$B" -G Ninja -DSSA_VERSION="$VERSION" > "$B/configure.log" 2>&1 \
    || { tail -30 "$B/configure.log"; exit 1; }
ninja -C "$B" -j"$JOBS" > "$B/build.log" 2>&1 \
    || { grep -E "error|undefined|FAILED" "$B/build.log" | head -60; exit 1; }

STAGE="$B/stage"
rm -rf "$STAGE"
mkdir -p "$STAGE/sce_sys"
cp "$B/ssa_eboot/eboot.bin" "$STAGE/eboot.bin"
cp "$HERE/sce_sys/icon0.png" "$STAGE/sce_sys/"
# the quick menu's fonts (Nunito, SIL Open Font License: OFL-Nunito.txt)
mkdir -p "$STAGE/fonts"
cp "$HERE/fonts/Nunito-ExtraBold.ttf" "$HERE/fonts/Nunito-Regular.ttf" "$HERE/fonts/OFL-Nunito.txt" "$STAGE/fonts/"
# SSA_PKG_FULL=dir: the one-file package, the game's files in it too - dir holds disc/ (the extracted
# disc), ssa16.aot and seed/ (nand/, figures/spyro.sky, settings.ini for the first start: ps4_main.cpp
# seedFromPackage), hard-linked into the stage (~4 GB: make-pkg.sh takes the kit's fixed PkgTool);
# named Skylanders-SSA-vXX.YY-full.pkg. The game's own data: for the user's consoles, never published.
NAME="Skylanders-SSA-v$VERSION.pkg"
if [ -n "${SSA_PKG_FULL:-}" ]; then
    for need in disc/sys/main.dol ssa16.aot seed; do
        [ -e "$SSA_PKG_FULL/$need" ] || { echo "SSA_PKG_FULL: no $SSA_PKG_FULL/$need"; exit 1; }
    done
    cp -al "$SSA_PKG_FULL"/. "$STAGE"/
    NAME="Skylanders-SSA-v$VERSION-full.pkg"
    echo "stage with the game's files: $(du -sh "$STAGE" | cut -f1)"
fi
PKG="$(NO_RIGHT_SPRX=1 bash "$PS4_KIT/scripts/make-pkg.sh" "$STAGE" SKYL00001 "Skylanders: Spyro's Adventure" "$VERSION" SKYLANDERSSA0001 "$B/out" | tail -1)"
OUT="$HERE/../out"
mkdir -p "$OUT" "$B/symbols"
cp "$PKG" "$OUT/$NAME"
# symbols of every packaged build (crash logs are symbolized against the exact binary)
cp "$B/ssa_eboot/ssa.oelf" "$B/symbols/ssa-v$VERSION.oelf"
cp "$B/ssa" "$B/symbols/ssa-v$VERSION.elf"
ls -la "$OUT/$NAME"
