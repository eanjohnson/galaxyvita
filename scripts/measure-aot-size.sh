#!/usr/bin/env bash
# Compile generated AOT C as objects (host + Vita) and print .text/.data.
# Does not link the game runtime. Generated C stays under generated/.
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
# shellcheck disable=SC1091
source "$ROOT/scripts/vita-env.sh"

SRC_DIR="${1:-}"
if [[ -z "$SRC_DIR" ]]; then
  if [[ -d "$ROOT/generated/aot/generated/chunks" ]]; then
    SRC_DIR="$ROOT/generated/aot/generated"
  else
    SRC_DIR=$(find "$ROOT/generated/aot" -type d -name '*_generated' 2>/dev/null | head -1 || true)
  fi
fi
if [[ -z "$SRC_DIR" || ! -d "$SRC_DIR" ]]; then
  echo "no generated C dir — run ./scripts/generate-aot.sh" >&2
  exit 1
fi

HOST_BUILD="$ROOT/generated/build/aot-host"
VITA_BUILD="$ROOT/generated/build/aot-vita"
CMAKE_SRC="$ROOT/generated/build/aot-src"
REPORT="$ROOT/generated/aot/SIZE.txt"
INC="$ROOT/third_party/DolRecomp/src"
JOBS="$(nproc)"

mkdir -p "$CMAKE_SRC"

# The flags the recompiled guest code is built with, spelled out.
#
# They used to be emergent rather than chosen. This script passed "-O2" and the
# vitasdk toolchain's Release configuration appended "-O3 -DNDEBUG" and then
# target_compile_options put "-O2" back on the end -- and gcc takes the last
# -O it is given, so the hottest code in the project quietly built at -O2 while
# the hand-written runtime, through the opposite ordering accident in the top
# level CMakeLists, built at -O3. Neither was a decision anybody made.
#
# GV_AOT_OPT and GV_AOT_ARCH are now the whole story, and the build type is
# pinned to empty so nothing gets appended behind them. The defaults reproduce
# exactly what shipped before this was noticed, so changing them is an
# experiment somebody ran, not a side effect: sweep with
#
#   GV_AOT_OPT=-O3 ./scripts/measure-aot-size.sh
#   GV_AOT_OPT=-Os ./scripts/measure-aot-size.sh
#   GV_AOT_ARCH="-mcpu=cortex-a9 -mfpu=neon -mfloat-abi=hard -marm" ...
#
# and note that every chunk function is about 47 KiB against a 32 KiB L1
# instruction cache, so "more inlining" is not obviously the faster direction.
# Measure it in VI/s on the device; .text alone does not say.
AOT_OPT="${GV_AOT_OPT:--O2}"
AOT_ARCH="${GV_AOT_ARCH:--mcpu=cortex-a9 -mfpu=neon -mfloat-abi=hard -mthumb}"
AOT_EXTRA="${GV_AOT_EXTRA:--fno-strict-aliasing}"
echo "==> AOT flags: $AOT_OPT $AOT_ARCH $AOT_EXTRA"

# CMake glob at configure time; reconfigure after generate-aot.sh.
cat > "$CMAKE_SRC/CMakeLists.txt" <<EOF
cmake_minimum_required(VERSION 3.16)
project(galaxyvita_aot_size C)
set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)
file(GLOB AOT_SOURCES "${SRC_DIR}/*.c" "${SRC_DIR}/chunks/*.c")
if(NOT AOT_SOURCES)
  message(FATAL_ERROR "no AOT C files in ${SRC_DIR}")
endif()
add_library(galaxyvita_aot OBJECT \${AOT_SOURCES})
target_include_directories(galaxyvita_aot PRIVATE "${INC}" "${SRC_DIR}")
target_compile_options(galaxyvita_aot PRIVATE ${AOT_OPT} ${AOT_EXTRA})
EOF

sum_size() {
  local sizebin="$1"
  shift
  "$sizebin" "$@" | awk '
    $1 ~ /^[0-9]+$/ { t+=$1; d+=$2; b+=$3 }
    END { printf "%d %d %d\n", t, d, b }
  '
}

echo "==> host objects"
cmake -S "$CMAKE_SRC" -B "$HOST_BUILD" -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build "$HOST_BUILD" -j"$JOBS"
mapfile -t HOST_OBJS < <(find "$HOST_BUILD" \( -name '*.o' -o -name '*.obj' \) \
  ! -name 'CMakeCCompilerABI.*' ! -name 'CMakeCCompilerId.*' | sort)
read -r HTEXT HDATA HBSS <<<"$(sum_size size "${HOST_OBJS[@]}")"

VTEXT=0; VDATA=0; VBSS=0
if [[ -z "${VITASDK:-}" || ! -x "$VITASDK/bin/arm-vita-eabi-gcc" ]]; then
  echo "VITASDK missing; skip Vita cross-compile" >&2
else
  echo "==> vita objects"
  # Build type deliberately empty: Release would append its own -O3 after
  # these and win, which is the bug this file used to have.
  #
  # The objects are globbed into the final link with no dependency edge back to
  # the flags they were built with, so a sweep that reused a tree built under
  # different flags would link a mixture and report it as one measurement.
  # Stamp the flags and wipe the tree when they move -- and only then, because
  # a full rebuild here is 1322 objects.
  STAMP="$VITA_BUILD/.gv-flags"
  WANT="$AOT_OPT $AOT_ARCH $AOT_EXTRA"
  if [[ ! -f "$STAMP" || "$(cat "$STAMP")" != "$WANT" ]]; then
    [[ -d "$VITA_BUILD" ]] && echo "==> AOT flags changed; rebuilding all objects"
    rm -rf "$VITA_BUILD"
  fi
  cmake -S "$CMAKE_SRC" -B "$VITA_BUILD" -G Ninja \
    -DCMAKE_BUILD_TYPE= \
    -DCMAKE_TOOLCHAIN_FILE="$VITASDK/share/vita.toolchain.cmake" \
    -DCMAKE_C_FLAGS="$AOT_OPT $AOT_ARCH $AOT_EXTRA"
  cmake --build "$VITA_BUILD" -j"$JOBS"
  printf '%s' "$WANT" > "$STAMP"
  mapfile -t VITA_OBJS < <(find "$VITA_BUILD" \( -name '*.o' -o -name '*.obj' \) \
    ! -name 'CMakeCCompilerABI.*' ! -name 'CMakeCCompilerId.*' | sort)
  read -r VTEXT VDATA VBSS <<<"$(sum_size arm-vita-eabi-size "${VITA_OBJS[@]}")"
fi

python3 - "$REPORT" "$SRC_DIR" "$HTEXT" "$HDATA" "$HBSS" "$VTEXT" "$VDATA" "$VBSS" <<'PY'
import pathlib, sys
report, src, htext, hdata, hbss, vtext, vdata, vbss = sys.argv[1:]
def fmt(n):
    n = int(n)
    return f"{n:12d} ({n/1024/1024:8.2f} MiB)"
lines = [
    f"source: {src}",
    f"host  text={fmt(htext)}  data={fmt(hdata)}  bss={fmt(hbss)}  sum={fmt(int(htext)+int(hdata)+int(hbss))}",
    f"vita  text={fmt(vtext)}  data={fmt(vdata)}  bss={fmt(vbss)}  sum={fmt(int(vtext)+int(vdata)+int(vbss))}",
]
vt = int(vtext)
gate = 280 * 1024 * 1024
lines.append(
    f"vita .text vs 280 MiB gate: {'PASS' if vt < gate else 'FAIL'} "
    f"({vt/1024/1024:.2f} MiB). Runtime estimate is recorded separately."
)
text = "\n".join(lines) + "\n"
pathlib.Path(report).write_text(text)
print(text, end="")
PY
