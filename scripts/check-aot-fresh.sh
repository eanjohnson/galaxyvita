#!/usr/bin/env bash
# Refuse to link prebuilt AOT objects that are older than the generated C.
#
# The Vita target links objects out of generated/build/aot-vita, which CMake
# only globs -- there is no dependency edge from an object back to the chunk it
# was compiled from. So a stale object links silently and the console runs code
# the host has already moved past. That is not hypothetical: one chunk missed a
# rebuild and the device spent two days spinning in a guest function the host
# had been answering with an HLE hook all along.
#
# measure-aot-size.sh skips the Vita half entirely when VITASDK is missing, and
# says so on stderr and nowhere else, which is how that object got left behind.
#
# The question "is any object out of date with respect to its own source" is
# exactly what ninja answers, and it answers it per file -- comparing mtimes
# here would either miss a stale object or condemn a fresh one.
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
OBJ="${1:-$ROOT/generated/build/aot-vita}"

[[ -f "$OBJ/build.ninja" ]] || exit 0
command -v ninja >/dev/null || exit 0

# "Re-running CMake" is not a stale object. ninja schedules it whenever the
# CMakeLists it was generated from has a newer mtime -- which measure-aot-size.sh
# causes every time it rewrites that file with identical content -- and until it
# has run, ninja cannot say whether any object is actually out of date. So run
# it, then ask again. It takes a fraction of a second, and without this the
# check fails on a fresh tree, which is the fastest way to teach someone to
# ignore it.
if ninja -C "$OBJ" -n 2>/dev/null | grep -q 'Re-running CMake'; then
  ninja -C "$OBJ" build.ninja >/dev/null 2>&1 || true
fi

pending=$(ninja -C "$OBJ" -n 2>/dev/null | grep -c '^\[' || true)
if [[ "$pending" -eq 0 ]]; then
  echo "check-aot-fresh: ok (${OBJ##*/} is up to date with the generated C)"
  exit 0
fi

echo "check-aot-fresh: $pending prebuilt Vita AOT object(s) are out of date" >&2
echo "  with the generated C. The VPK would ship stale code." >&2
ninja -C "$OBJ" -n 2>/dev/null | grep '^\[' | head -6 | sed 's/^/    /' >&2
echo "  Run ./scripts/measure-aot-size.sh to rebuild them." >&2
exit 1
