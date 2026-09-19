#!/usr/bin/env bash
# Linux smoke of the Phase 3 chassis. Does not touch the Vita.
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
DOL="${1:-$ROOT/generated/extracted/identity/sys/main.dol}"
CAP="${2:-50000}"
OUT="$ROOT/generated/build/host-chassis"
mkdir -p "$OUT"

# Which tree of AOT objects to link. Overridable so two recompiler variants can
# be compared without either of them clobbering the other's objects -- the
# globbed default has no dependency edge back to the C it came from, which is
# how a stale object once passed for a host/Vita divergence.
AOT_HOST_DIR="${GV_AOT_HOST_DIR:-$ROOT/generated/build/aot-host}"
GEN_DIR_PROBE="${GV_AOT_GEN_DIR:-$ROOT/generated/aot/generated}"
mapfile -t OBJS < <(find "$AOT_HOST_DIR" \( -name '*.o' -o -name '*.obj' \) \
  ! -name 'CMakeCCompilerABI.*' ! -name 'CMakeCCompilerId.*' | sort)
if [[ ${#OBJS[@]} -eq 0 ]]; then
  echo "no host AOT objects in $AOT_HOST_DIR — run ./scripts/measure-aot-size.sh first" >&2
  exit 1
fi

# An object whose source no longer exists is stale by definition, and this find
# is what picks it up: deleting a chunk, or a scratch file that was never meant
# to be compiled, leaves the object behind and the link takes it. It happened.
# An experiment wrote two intermediates into the chunks directory, CMake's glob
# built them, the sources were then removed and the build graph correctly forgot
# them -- and this link still found the objects and failed on a duplicate
# symbol, forty minutes after the rebuild that was supposed to have cleaned up.
# A duplicate symbol is the loud case; an orphan that does not collide would
# have linked silently and run code with no source in the tree.
ORPHANS=()
for o in "${OBJS[@]}"; do
  b="$(basename "$o")"; b="${b%.o}"; b="${b%.obj}"
  [[ -f "$GEN_DIR_PROBE/$b" || -f "$GEN_DIR_PROBE/chunks/$b" ]] || ORPHANS+=("$b")
done
if [[ ${#ORPHANS[@]} -gt 0 ]]; then
  echo "refusing to link: ${#ORPHANS[@]} host AOT object(s) have no source:" >&2
  printf '  %s\n' "${ORPHANS[@]:0:8}" >&2
  echo "  Delete them from $AOT_HOST_DIR and rebuild." >&2
  exit 1
fi

MG="$ROOT/third_party/ModernGekko"
GEN_DIR="${GV_AOT_GEN_DIR:-$ROOT/generated/aot/generated}"
CINC=(-I"$ROOT/runtime" -I"$ROOT/tests/host" -I"$ROOT/third_party/DolRecomp/src"
      -I"$GEN_DIR")
# The command processor is ModernGekko's, so the chassis is a mixed C/C++ link
# now: C sources stay on gcc, the GPU stack builds with g++, and g++ links.
CXXINC=("${CINC[@]}" -I"$MG/include" -I"$MG/vendor/dolphin_legacy"
        -I"$MG/vendor/picojson" -I"$MG/src/dolphin_shims"
        -I"$MG/vendor/fmt/fmt/include" -DFMT_HEADER_ONLY=1)

CSRC=(
  "$ROOT/tests/host/aot_chassis.c"
  "$ROOT/tests/host/gx_soft.c"
  "$ROOT/runtime/gx_texture.c"
  "$ROOT/runtime/chassis.c"
  "$ROOT/runtime/flipper.c"
  "$ROOT/runtime/gx_queue.c"
  "$ROOT/runtime/ios.c"
  "$ROOT/runtime/bt.c"
  "$ROOT/runtime/disc.c"
  "$ROOT/runtime/nand.c"
  "$ROOT/third_party/DolRecomp/src/cpu/cpu.c"
  "$ROOT/third_party/DolRecomp/src/frontend/container/dol.c"
)
CXXSRC=(
  "$ROOT/runtime/gx_cp.cpp"
  "$MG/src/memory/address_space.cpp"
  "$MG/src/gpu/gx_command_processor.cpp"
  "$MG/src/gpu/gx_state_backend.cpp"
  "$MG/src/gpu/gx_vertex_loader.cpp"
)

rm -rf "$OUT/obj"; mkdir -p "$OUT/obj"
COBJS=()
for f in "${CSRC[@]}"; do
  o="$OUT/obj/$(basename "${f%.c}").o"
  gcc -O2 -std=c11 -fno-strict-aliasing "${CINC[@]}" -c "$f" -o "$o"
  COBJS+=("$o")
done
for f in "${CXXSRC[@]}"; do
  o="$OUT/obj/$(basename "${f%.cpp}").opp"
  g++ -O2 -std=c++23 "${CXXINC[@]}" -c "$f" -o "$o"
  COBJS+=("$o")
done

g++ -O2 -o "$OUT/aot_chassis" "${COBJS[@]}" "${OBJS[@]}" -lm -lpthread

echo "==> $OUT/aot_chassis $DOL cap=$CAP"
"$OUT/aot_chassis" "$DOL" "$CAP"
