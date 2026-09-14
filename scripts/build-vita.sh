#!/usr/bin/env bash
# Configure and build the current Vita target (hello-world host until the runtime lands).
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
# shellcheck disable=SC1091
source "$ROOT/scripts/vita-env.sh"

if [[ -z "${VITASDK:-}" || ! -x "$VITASDK/bin/arm-vita-eabi-gcc" ]]; then
  echo "VITASDK is not set or arm-vita-eabi-gcc is missing. Run ./scripts/bootstrap-host.sh" >&2
  exit 1
fi

BUILD="$ROOT/generated/build/vita"
mkdir -p "$BUILD"

cmake -S "$ROOT" -B "$BUILD" -G Ninja \
  -DCMAKE_BUILD_TYPE=Release \
  -DCMAKE_TOOLCHAIN_FILE="$VITASDK/share/vita.toolchain.cmake"

cmake --build "$BUILD"

echo "Artifacts under $BUILD"
ls -lh "$BUILD"/*.vpk "$BUILD"/*.self "$BUILD"/eboot.bin 2>/dev/null || ls -lh "$BUILD"
