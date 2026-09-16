#!/usr/bin/env bash
# Does ModernGekko's GX stack cross-compile for the Vita?
#
# This is the gate for the graphics architecture: if Dolphin's shader
# generators and ModernGekko's GX layer build for arm-vita-eabi, the Vita port
# is a GpuBackend implementation rather than a reimplementation of GX.
#
# Does not touch the Vita and does not link -- compile only.
set -uo pipefail
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
MG="$ROOT/third_party/ModernGekko"
: "${VITASDK:=$HOME/vitasdk}"
export VITASDK PATH="$VITASDK/bin:$PATH"

CXX=arm-vita-eabi-g++
command -v "$CXX" >/dev/null || {
  echo "no $CXX on PATH -- set VITASDK (currently '$VITASDK')" >&2
  exit 1
}

OUT="$(mktemp -d)"
trap 'rm -rf "$OUT"' EXIT
# Mirrors the moderngekko_dolphin_video target in ModernGekko/CMakeLists.txt:
# C++23, and src/dolphin_shims stands in for the Core/ headers VideoCommon
# pulls in but does not need.
INC=(-I "$MG/include" -I "$MG/vendor/dolphin_legacy" -I "$MG/vendor/picojson"
     -I "$MG/src/dolphin_shims" -I "$MG/vendor/fmt/fmt/include"
     -DFMT_HEADER_ONLY=1)

SOURCES=(
  vendor/dolphin_legacy/VideoCommon/BPMemory.cpp
  vendor/dolphin_legacy/VideoCommon/XFMemory.cpp
  vendor/dolphin_legacy/VideoCommon/ShaderGenCommon.cpp
  vendor/dolphin_legacy/VideoCommon/LightingShaderGen.cpp
  vendor/dolphin_legacy/VideoCommon/PixelShaderGen.cpp
  vendor/dolphin_legacy/VideoCommon/VertexShaderGen.cpp
  vendor/dolphin_legacy/VideoCommon/GeometryShaderGen.cpp
  vendor/dolphin_legacy/VideoCommon/UberShaderCommon.cpp
  vendor/dolphin_legacy/VideoCommon/UberShaderPixel.cpp
  vendor/dolphin_legacy/VideoCommon/UberShaderVertex.cpp
  vendor/dolphin_legacy/VideoCommon/DriverDetails.cpp
  src/gpu/dolphin_shader_compiler.cpp
  src/gpu/dolphin_video_host.cpp
  src/gpu/gx_command_processor.cpp
  src/gpu/gx_pipeline.cpp
  src/gpu/gx_state_backend.cpp
  src/gpu/gx_texture_decoder.cpp
  src/gpu/gx_vertex_loader.cpp
)

fail=0
for src in "${SOURCES[@]}"; do
  name="$(basename "$src" .cpp)"
  if "$CXX" -std=c++23 -O2 -c "${INC[@]}" -o "$OUT/$name.o" "$MG/$src" \
      2> "$OUT/$name.log"; then
    printf '  %-26s ok\n' "$name"
  else
    printf '  %-26s FAIL\n' "$name"
    sed -n '1,5p' "$OUT/$name.log" | sed 's/^/      /'
    fail=1
  fi
done

[[ $fail -eq 0 ]] && echo "probe-vita-gx: ok ($(( ${#SOURCES[@]} )) objects)" \
                  || echo "probe-vita-gx: FAILED"
exit $fail
