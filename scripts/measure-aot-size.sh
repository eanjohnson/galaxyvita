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

# Nothing but generated chunks may live in the chunks directory.
#
# CMake globs it and there is no dependency edge from an object back to
# anything, so a scratch file dropped here silently becomes part of the build
# and links into the eboot. An experiment comparing two compilations of one
# chunk left its two intermediates behind -- a command that would have deleted
# them timed out before it got there -- and the next build compiled a third and
# fourth copy of the same guest functions. That one failed loudly, on a
# duplicate symbol; a scratch file that happened to compile would not have.
CHUNKS="$ROOT/generated/aot/generated/chunks"
if [[ -d "$CHUNKS" ]]; then
  stray=$(find "$CHUNKS" -maxdepth 1 -name '*.c' ! -name 'chunk_*.c' -printf '%f\n')
  if [[ -n "$stray" ]]; then
    echo "refusing to build: files in chunks/ that dolrecomp did not generate:" >&2
    echo "$stray" | sed 's/^/  /' >&2
    exit 1
  fi
fi

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

# Which halves to build. Both by default, because the host objects are what
# host equivalence runs against and skipping them silently is how a gate stops
# being run.
#
# But a flag or arch sweep only ever asks "is this faster on the console", and
# the host half of that answer is 42 of the 80 minutes and 250 MB for nothing.
# GV_AOT_TARGETS=vita halves those experiments:
#
#   GV_AOT_TARGETS=vita GV_AOT_ARCH="... -marm" ./scripts/measure-aot-size.sh
#
# Note what that costs: with no host objects there is no host equivalence gate,
# so a vita-only build can only be checked on the device, by the boot reaching
# the same retraces. That is weaker evidence. Use it for flags that cannot
# change behaviour, not for emitter changes that can.
AOT_TARGETS="${GV_AOT_TARGETS:-both}"
case "$AOT_TARGETS" in
  both|host|vita) ;;
  *) echo "GV_AOT_TARGETS must be both, host or vita (got '$AOT_TARGETS')" >&2; exit 1 ;;
esac
[[ "$AOT_TARGETS" == both ]] || echo "==> building $AOT_TARGETS objects only"

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

HTEXT=0; HDATA=0; HBSS=0
if [[ "$AOT_TARGETS" == vita ]]; then
  echo "==> host objects skipped (GV_AOT_TARGETS=vita); host equivalence cannot run"
else
  echo "==> host objects"
  cmake -S "$CMAKE_SRC" -B "$HOST_BUILD" -G Ninja -DCMAKE_BUILD_TYPE=Release
  cmake --build "$HOST_BUILD" -j"$JOBS"
  mapfile -t HOST_OBJS < <(find "$HOST_BUILD" \( -name '*.o' -o -name '*.obj' \) \
    ! -name 'CMakeCCompilerABI.*' ! -name 'CMakeCCompilerId.*' | sort)
  read -r HTEXT HDATA HBSS <<<"$(sum_size size "${HOST_OBJS[@]}")"
fi

VTEXT=0; VDATA=0; VBSS=0
if [[ "$AOT_TARGETS" == host ]]; then
  echo "==> vita objects skipped (GV_AOT_TARGETS=host)"
elif [[ -z "${VITASDK:-}" || ! -x "$VITASDK/bin/arm-vita-eabi-gcc" ]]; then
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
    # Park the current tree under its own flags rather than deleting it, and
    # bring back the one for these flags if it has been built before.
    #
    # A sweep revisits the same flag sets constantly -- every variant is
    # compared against the -O2/-mthumb baseline -- and rebuilding that baseline
    # each time was 38 minutes of recompiling bytes we already had. Ninja skips
    # what has not changed, but only if the tree still exists to skip against.
    #
    # The stamp lives INSIDE each parked tree, so a tree can only ever be
    # restored under the flags it was built with. That is the whole safety
    # argument: the thing this replaces existed to stop a mixed-flag tree being
    # reported as one measurement, and parking must not reintroduce it.
    PARK="$ROOT/generated/build/aot-vita-flags"
    mkdir -p "$PARK"
    key() { printf '%s' "$1" | md5sum | cut -c1-12; }
    if [[ -f "$STAMP" ]]; then
      OLD_KEY="$(key "$(cat "$STAMP")")"
      rm -rf "${PARK:?}/$OLD_KEY"
      mv "$VITA_BUILD" "$PARK/$OLD_KEY"
      echo "==> parked the previous tree as $OLD_KEY ($(cat "$PARK/$OLD_KEY/.gv-flags"))"
    fi
    rm -rf "$VITA_BUILD"
    NEW_KEY="$(key "$WANT")"
    if [[ -f "$PARK/$NEW_KEY/.gv-flags" && "$(cat "$PARK/$NEW_KEY/.gv-flags")" == "$WANT" ]]; then
      mv "$PARK/$NEW_KEY" "$VITA_BUILD"
      echo "==> restored a tree previously built with these flags; ninja will rebuild only what moved"
    else
      # Refuse a parked tree whose stamp disagrees rather than trusting the key.
      rm -rf "${PARK:?}/$NEW_KEY"
      echo "==> AOT flags changed and no tree exists for them; building all objects"
    fi
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

python3 - "$REPORT" "$SRC_DIR" "$HTEXT" "$HDATA" "$HBSS" "$VTEXT" "$VDATA" "$VBSS" "$AOT_TARGETS" <<'PY'
import pathlib, sys
report, src, htext, hdata, hbss, vtext, vdata, vbss, targets = sys.argv[1:]
def fmt(n):
    n = int(n)
    return f"{n:12d} ({n/1024/1024:8.2f} MiB)"
built_host = targets in ("both", "host")
built_vita = targets in ("both", "vita")
lines = [f"source: {src}"]
# A half that was not built says so. Reporting it as 0.00 MiB is the same
# mistake as the clock line that claimed a frequency nobody had set: it reads
# as a measurement and is the absence of one.
lines.append(
    f"host  text={fmt(htext)}  data={fmt(hdata)}  bss={fmt(hbss)}  sum={fmt(int(htext)+int(hdata)+int(hbss))}"
    if built_host else "host  not built this run (GV_AOT_TARGETS=%s)" % targets)
lines.append(
    f"vita  text={fmt(vtext)}  data={fmt(vdata)}  bss={fmt(vbss)}  sum={fmt(int(vtext)+int(vdata)+int(vbss))}"
    if built_vita else "vita  not built this run (GV_AOT_TARGETS=%s)" % targets)
vt = int(vtext)
gate = 280 * 1024 * 1024
if not built_vita or vt == 0:
    # Never let the size gate PASS on an empty measurement.
    lines.append("vita .text vs 280 MiB gate: NOT RUN (no Vita objects this run).")
else:
    lines.append(
        f"vita .text vs 280 MiB gate: {'PASS' if vt < gate else 'FAIL'} "
        f"({vt/1024/1024:.2f} MiB). Runtime estimate is recorded separately."
    )
text = "\n".join(lines) + "\n"
pathlib.Path(report).write_text(text)
print(text, end="")
PY
