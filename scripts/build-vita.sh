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

BUILD="${GV_VITA_BUILD:-$ROOT/generated/build/vita}"
# Which AOT trees to link. They travel as a pair; see the comment in
# CMakeLists.txt. Empty means the defaults baked into CMakeLists.
# if-blocks and not `[[ ... ]] && ...`: under `set -e` a trailing test that
# fails is the script's exit status, so the unset case would abort the build.
AOT_ARGS=()
if [[ -n "${GV_AOT_GEN_DIR:-}" ]]; then
  AOT_ARGS+=(-DAOT_GEN_DIR="$GV_AOT_GEN_DIR")
fi
if [[ -n "${GV_AOT_OBJ_DIR:-}" ]]; then
  AOT_ARGS+=(-DAOT_OBJ_DIR="$GV_AOT_OBJ_DIR")
fi
mkdir -p "$BUILD"

# vita-elf-create appends about 1.8 KB of SCE module data at the end of the
# read-only segment and aborts -- "Cannot allocate N bytes for SCE data at end
# of segment 0" -- when that segment ends within 1.8 KB of a page boundary.
# Where it ends is otherwise arbitrary, so about half of all builds fail this
# way for a reason that has nothing to do with the change being built. Pad the
# segment and rebuild until it fits, rather than leaving a coin flip in the
# build.
PAD="${GALAXYVITA_LINK_PAD:-0}"
built=0
for _ in $(seq 1 16); do
  cmake -S "$ROOT" -B "$BUILD" -G Ninja \
    -DCMAKE_BUILD_TYPE=Release \
    -DGALAXYVITA_LINK_PAD="$PAD" \
    ${AOT_ARGS[@]+"${AOT_ARGS[@]}"} \
    -DCMAKE_TOOLCHAIN_FILE="$VITASDK/share/vita.toolchain.cmake"

  if cmake --build "$BUILD" 2>&1 | tee "$BUILD/build.log"; then
    built=1
    if [[ "$PAD" -gt 0 ]]; then
      echo "==> needed ${PAD} bytes of segment padding"
    fi
    break
  fi
  grep -q 'Cannot allocate .* for SCE data' "$BUILD/build.log" || {
    echo "build failed; see $BUILD/build.log" >&2
    exit 1
  }
  PAD=$((PAD + 512))
  echo "==> segment too tight for vita-elf-create; retrying with pad=$PAD"
done
[[ "$built" -eq 1 ]] || { echo "could not pad the segment enough" >&2; exit 1; }

echo "Artifacts under $BUILD"
ls -lh "$BUILD"/*.vpk "$BUILD"/*.self "$BUILD"/eboot.bin 2>/dev/null || ls -lh "$BUILD"
