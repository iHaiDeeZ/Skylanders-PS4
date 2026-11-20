#!/bin/bash
# Extracts the DATA partition of your Skylanders: Spyro's Adventure disc image (sys/, files/, ...):
#   tools/extract-disc.sh <your disc image: .iso, .rvz, .wbfs ...> <out dir>
# The out dir is what goes to the console as /data/SSA/disc (or into the one-file package's disc/).
# DOLPHIN_TOOL: dolphin-tool (from a Dolphin build, e.g. its dolphin-tool target)
set -e
T=${DOLPHIN_TOOL:-dolphin-tool}
DISC="${1:?your own disc image (.iso, .rvz, .wbfs)}"
OUT="${2:?out dir}"
mkdir -p "$OUT"
"$T" extract -i "$DISC" -o "$OUT" -g -q
echo "done"
find "$OUT" -maxdepth 2 | head -50
du -sh "$OUT"
