#!/bin/bash
# Prints your Skylanders disc image's header and dolphin-tool's extract options.
#   tools/disc-info.sh <your disc image: .iso, .rvz, .wbfs ...>
# DOLPHIN_TOOL: dolphin-tool (from a Dolphin build, e.g. its dolphin-tool target)
T=${DOLPHIN_TOOL:-dolphin-tool}
DISC="${1:?your own disc image (.iso, .rvz, .wbfs)}"
"$T" header -i "$DISC"
"$T" extract --help 2>&1 | head -30
