#!/usr/bin/env bash
# Generates the shaders Dolphin would emit for captured SMG draw states, with
# the Vita's capability flags set. Host-only; does not touch the Vita.
#
#   ./scripts/run-host-chassis.sh ...   with GV_GFX_STATEDUMP=<dir>
#   ./scripts/dump-dolphin-shaders.sh <state-dir> [out-dir]
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
MG="$ROOT/third_party/ModernGekko"
STATES="${1:?state dir (from GV_GFX_STATEDUMP)}"
OUT="${2:-$ROOT/generated/build/dolphin-shaders}"
BUILD="$ROOT/generated/build/shader-probe"
mkdir -p "$BUILD" "$OUT"

INC=(-I "$MG/include" -I "$MG/vendor/dolphin_legacy" -I "$MG/vendor/picojson"
     -I "$MG/src/dolphin_shims" -I "$MG/vendor/fmt/fmt/include"
     -DFMT_HEADER_ONLY=1)

SOURCES=(
  "$ROOT/tests/host/shader_probe.cpp"
  "$MG/src/gpu/dolphin_shader_compiler.cpp"
  "$MG/src/gpu/dolphin_video_host.cpp"
  "$MG/vendor/dolphin_legacy/VideoCommon/BPMemory.cpp"
  "$MG/vendor/dolphin_legacy/VideoCommon/XFMemory.cpp"
  "$MG/vendor/dolphin_legacy/VideoCommon/ShaderGenCommon.cpp"
  "$MG/vendor/dolphin_legacy/VideoCommon/LightingShaderGen.cpp"
  "$MG/vendor/dolphin_legacy/VideoCommon/PixelShaderGen.cpp"
  "$MG/vendor/dolphin_legacy/VideoCommon/VertexShaderGen.cpp"
  "$MG/vendor/dolphin_legacy/VideoCommon/GeometryShaderGen.cpp"
  "$MG/vendor/dolphin_legacy/VideoCommon/DriverDetails.cpp"
  "$MG/vendor/dolphin_legacy/VideoCommon/UberShaderCommon.cpp"
  "$MG/vendor/dolphin_legacy/VideoCommon/UberShaderPixel.cpp"
  "$MG/vendor/dolphin_legacy/VideoCommon/UberShaderVertex.cpp"
)

g++ -std=c++23 -O1 -g "${INC[@]}" -o "$BUILD/shader_probe" "${SOURCES[@]}"
"$BUILD/shader_probe" "$STATES" "$OUT"
