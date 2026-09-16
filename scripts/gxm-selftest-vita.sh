#!/usr/bin/env bash
# Answers the question the host cannot: does the raw sceGxm path work on the
# console -- console-compiled Cg bound through the shader patcher, GX clip-space
# vertices, the right pixels in the right places?
#
# Builds a standalone VPK from vita/gfx/, runs it over vitacompanion and fetches
# both the report and the presented frame. No interaction on the console.
#
#   ./scripts/gxm-selftest-vita.sh
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
BUILD="$ROOT/generated/build/gxm-selftest"
TITLE="${GALAXYVITA_TITLEID:-GVITA0001}"
# shellcheck disable=SC1091
source "$ROOT/scripts/vita-env.sh"
: "${VITASDK:=$HOME/vitasdk}"
export VITASDK PATH="$VITASDK/bin:$PATH"

# One client at a time. Two concurrent runs wedge vitacompanion's FTP and each
# one's restore trap fights the other's staging.
exec 9>"${TMPDIR:-/tmp}/galaxyvita-device.lock"
flock -n 9 || { echo "another script is using the Vita; wait for it or kill it" >&2; exit 1; }

[[ -n "${GALAXYVITA_IP:-}" ]] || { echo "Set GALAXYVITA_IP or write scripts/.vita-ip" >&2; exit 1; }
IP="$GALAXYVITA_IP"
cmd() { printf '%s\n' "$1" | socat - "TCP:$IP:1338,connect-timeout=5"; }

mkdir -p "$BUILD"

# -Wl,-q is mandatory: vita-elf-create needs the relocations and accepts an ELF
# without them silently; the result launches, jumps into padding and dies
# before main with no diagnostic.
#
# The pad is the second trap in the same step. vita-elf-create appends about
# 1.8 KB of SCE module data at the end of the read-only segment, and if that
# segment happens to end within 1.8 KB of the next page boundary there is no
# room: "Cannot allocate N bytes for SCE data at end of segment 0". Where the
# segment ends is otherwise arbitrary, so roughly half of all builds fail this
# way -- for a reason that has nothing to do with the change being built.
# Padding the segment out and retrying is deterministic and needs no source
# change, so the link simply grows the pad until it fits.
link() {
  local pad="$1"
  if [[ "$pad" -gt 0 ]]; then
    printf '\t.section .rodata\n\t.space %d, 0\n' "$pad" > "$BUILD/pad.s"
  else
    : > "$BUILD/pad.s"
  fi
  arm-vita-eabi-gcc -O2 -Wall -Wextra -std=c11 -Wl,-q -o "$BUILD/gxm_selftest.elf" \
    -I"$ROOT/runtime" -I"$ROOT/vita/gfx" -I"$ROOT/third_party/DolRecomp/src" \
    "$ROOT/vita/gfx/gxm_selftest.c" \
    "$ROOT/vita/gfx/gxm.c" \
    "$ROOT/vita/gfx/gx_gxm.c" \
    "$ROOT/vita/gfx/shader_cache.c" \
    "$BUILD/pad.s" \
    -lSceShaccCg_stub -lSceGxm_stub -lSceDisplay_stub \
    -lSceSysmodule_stub -lSceAppMgr_stub -lSceCommonDialog_stub \
    -lSceKernelDmacMgr_stub -lSceIofilemgr_stub -lSceSysmem_stub \
    -lSceLibKernel_stub -lSceKernelThreadMgr_stub -lm
}

pad=0
built=0
for _ in $(seq 1 16); do
  link "$pad"
  # A count of 0 here is the silent -Wl,-q failure above, not a pad problem.
  rels=$(arm-vita-eabi-readelf -S "$BUILD/gxm_selftest.elf" | grep -c 'REL ' || true)
  [[ "$rels" -gt 0 ]] || { echo "no relocations: -Wl,-q did not take" >&2; exit 1; }
  if vita-elf-create "$BUILD/gxm_selftest.elf" "$BUILD/gxm_selftest.velf" 2>"$BUILD/velf.err"; then
    built=1
    echo "==> linked (pad=$pad, $rels relocation sections)"
    break
  fi
  grep -q 'Cannot allocate' "$BUILD/velf.err" || { cat "$BUILD/velf.err" >&2; exit 1; }
  pad=$((pad + 512))
done
[[ "$built" -eq 1 ]] || { echo "could not pad the segment enough for vita-elf-create" >&2; exit 1; }

# Unsafe: ShaccCg returns "fatal internal error" for every program when the app
# is a safe fself -- the module loads but the compiler cannot do its work.
vita-make-fself "$BUILD/gxm_selftest.velf" "$BUILD/eboot.bin"

ftp() { curl -sS --connect-timeout 10 "$@"; }
put() { ftp --ftp-create-dirs -T "$1" "ftp://$IP:1337/$2"; }
del() { ftp -Q "-DELE $1" "ftp://$IP:1337/ux0:/data/galaxyvita/" >/dev/null 2>&1 || true; }

# This vitacompanion has no `install`, so the test borrows the project's own
# already-registered slot. Back the real eboot up first and put it back after.
BACKUP="$BUILD/eboot.bin.$TITLE.bak"
if [[ ! -f "$BACKUP" ]]; then
  echo "==> backing up $TITLE eboot"
  ftp -o "$BACKUP" "ftp://$IP:1337/ux0:/app/$TITLE/eboot.bin"
fi
restore() {
  [[ -f "$BACKUP" ]] || return 0
  echo "==> restoring $TITLE eboot"
  cmd "destroy" >/dev/null 2>&1 || true
  for attempt in 1 2 3 4; do
    sleep $(( attempt * 3 ))
    if put "$BACKUP" "ux0:/app/$TITLE/eboot.bin"; then
      return 0
    fi
    echo "   restore attempt $attempt failed; retrying" >&2
  done
  echo "RESTORE FAILED -- $BACKUP holds the original" >&2
}
trap restore EXIT

echo "==> staging to $IP"
cmd "screen on" >/dev/null || true
cmd "nosleep on" >/dev/null || true
cmd "destroy" >/dev/null || true
put "$BUILD/eboot.bin" "ux0:/app/$TITLE/eboot.bin"

# GV_GXM_FRESH=1 drops the cached GXP so the compile path is exercised rather
# than the load path. Without it the second run onward only proves the cache.
if [[ -n "${GV_GXM_FRESH:-}" ]]; then
  for old in $(ftp --list-only "ftp://$IP:1337/ux0:/data/galaxyvita/gxp/" 2>/dev/null); do
    del "ux0:/data/galaxyvita/gxp/$old"
  done
  echo "==> cleared the GXP cache"
fi

# The marker's reappearance is the only reliable signal that this launch ran,
# as opposed to the previous launch's report still sitting on disk.
del "ux0:/data/galaxyvita/gxm_batch.txt"
cmd "launch $TITLE" >/dev/null || true

echo "==> waiting for the report"
got=0
for _ in $(seq 1 30); do
  sleep 5
  if ftp -o "$BUILD/mark.txt" \
       "ftp://$IP:1337/ux0:/data/galaxyvita/gxm_batch.txt" 2>/dev/null; then
    got=1
    break
  fi
done

ftp -o "$BUILD/report.txt" \
  "ftp://$IP:1337/ux0:/data/galaxyvita/gxm_selftest.txt" 2>/dev/null || true
ftp -o "$BUILD/frame.raw" \
  "ftp://$IP:1337/ux0:/data/galaxyvita/gxm_frame.raw" 2>/dev/null || true

if [[ -f "$BUILD/frame.raw" ]]; then
  python3 "$ROOT/scripts/raw_to_png.py" "$BUILD/frame.raw" "$BUILD/frame.png" 960 544
  echo "==> frame at $BUILD/frame.png"
fi

cat "$BUILD/report.txt" 2>/dev/null || echo "no report written" >&2
[[ "$got" -eq 1 ]] || { echo "the app never finished" >&2; exit 1; }
