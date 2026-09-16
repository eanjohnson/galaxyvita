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
target_compile_options(galaxyvita_aot PRIVATE -O2 -fno-strict-aliasing)
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
  cmake -S "$CMAKE_SRC" -B "$VITA_BUILD" -G Ninja \
    -DCMAKE_BUILD_TYPE=Release \
    -DCMAKE_TOOLCHAIN_FILE="$VITASDK/share/vita.toolchain.cmake" \
    -DCMAKE_C_FLAGS="-O2 -fno-strict-aliasing -mcpu=cortex-a9 -mfpu=neon -mfloat-abi=hard -mthumb"
  cmake --build "$VITA_BUILD" -j"$JOBS"
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
