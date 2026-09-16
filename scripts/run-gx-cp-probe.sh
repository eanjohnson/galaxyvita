#!/usr/bin/env bash
# Direct check of the command-processor swap. Hand-builds a FIFO stream and
# confirms the decoded geometry, the register mirror and the Pixel Engine
# events come out the way the rasteriser and the Flipper expect.
#
# Fast enough to run on every change, unlike the full boot -- which is the
# other half of the check, and the only one that exercises real geometry.
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
MG="$ROOT/third_party/ModernGekko"
OUT="$ROOT/generated/build/gx-cp-probe"
rm -rf "$OUT"; mkdir -p "$OUT"

CXXINC=(-I"$ROOT/runtime" -I"$ROOT/third_party/DolRecomp/src" -I"$MG/include"
        -I"$MG/vendor/dolphin_legacy" -I"$MG/vendor/picojson"
        -I"$MG/src/dolphin_shims" -I"$MG/vendor/fmt/fmt/include"
        -DFMT_HEADER_ONLY=1)

gcc -O1 -std=c11 -Wall -Wextra -I"$ROOT/runtime" -I"$ROOT/third_party/DolRecomp/src" \
  -c "$ROOT/tests/host/gx_cp_probe.c" -o "$OUT/probe.o"
OBJS=("$OUT/probe.o")
for f in "$ROOT/runtime/gx_cp.cpp" \
         "$MG/src/memory/address_space.cpp" \
         "$MG/src/gpu/gx_command_processor.cpp" \
         "$MG/src/gpu/gx_state_backend.cpp" \
         "$MG/src/gpu/gx_vertex_loader.cpp"; do
  o="$OUT/$(basename "${f%.cpp}").opp"
  g++ -O1 -std=c++23 "${CXXINC[@]}" -c "$f" -o "$o"
  OBJS+=("$o")
done
g++ -o "$OUT/gx_cp_probe" "${OBJS[@]}"
"$OUT/gx_cp_probe"
