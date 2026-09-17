#!/usr/bin/env bash
# How long does an application get to live on this console?
#
# Every GalaxyVita boot dies 10-12 seconds in, and an idle run that emulates
# nothing dies the same way -- so the cause is not in the chassis. This runs
# the smallest app that can still report, through the same path, in the same
# slot, as the same unsafe fself. Sixty ticks means the app was left alone and
# GalaxyVita is doing something; ten means the device or the launch is.
#
#   ./scripts/lifetime-probe-vita.sh              # bare control
#   GV_PROBE_MEM=1 ./scripts/lifetime-probe-vita.sh      # + 88 MB USER_RW
#   GV_PROBE_STACK=1 ./scripts/lifetime-probe-vita.sh    # + an 8 MB thread
#   GV_PROBE_THREADS=1 ./scripts/lifetime-probe-vita.sh  # + pinned workers
#
# The flags stack, and each adds one thing the real app does. Add them one at
# a time and watch for the run that stops reaching sixty.
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
BUILD="$ROOT/generated/build/lifetime-probe"
# shellcheck disable=SC1091
source "$ROOT/scripts/vita-env.sh"
: "${VITASDK:=$HOME/vitasdk}"
export VITASDK PATH="$VITASDK/bin:$PATH"
TITLE="${GALAXYVITA_TITLEID:-GVITA0001}"

# One client at a time; two runners wedge vitacompanion and cost a power cycle.
exec 9>"${TMPDIR:-/tmp}/galaxyvita-device.lock"
flock -n 9 || { echo "another script is using the Vita; wait for it or kill it" >&2; exit 1; }

[[ -n "${GALAXYVITA_IP:-}" ]] || { echo "Set GALAXYVITA_IP or write scripts/.vita-ip" >&2; exit 1; }
IP="$GALAXYVITA_IP"
cmd() { printf '%s\n' "$1" | socat - "TCP:$IP:1338,connect-timeout=5"; }
ftp() { curl -sS --connect-timeout 10 "$@"; }
put() { ftp --ftp-create-dirs -T "$1" "ftp://$IP:1337/$2"; }
del() { ftp -Q "-DELE $1" "ftp://$IP:1337/ux0:/data/galaxyvita/" >/dev/null 2>&1 || true; }

mkdir -p "$BUILD"

# -Wl,-q is mandatory: vita-elf-create accepts an ELF with no relocations
# silently and the result jumps into padding before main. The pad is the second
# trap in the same step -- vita-elf-create appends ~1.8 KB of SCE data at the
# end of the read-only segment and aborts when that segment ends within 1.8 KB
# of a page boundary, which is about half of all builds for reasons that have
# nothing to do with the source. Grow the pad and retry.
link() {
  local pad="$1"
  if [[ "$pad" -gt 0 ]]; then
    printf '\t.section .rodata\n\t.space %d, 0\n' "$pad" > "$BUILD/pad.s"
  else
    : > "$BUILD/pad.s"
  fi
  arm-vita-eabi-gcc -O2 -Wall -Wextra -std=c11 -Wl,-q -o "$BUILD/probe.elf" \
    "$ROOT/vita/host/lifetime_probe.c" "$BUILD/pad.s" \
    -lSceIofilemgr_stub -lSceSysmem_stub -lSceLibKernel_stub \
    -lSceKernelThreadMgr_stub -lSceProcessmgr_stub -lSceDisplay_stub -lSceCtrl_stub
}

pad=0
built=0
for _ in $(seq 1 16); do
  link "$pad"
  rels=$(arm-vita-eabi-readelf -S "$BUILD/probe.elf" | grep -c 'REL ' || true)
  [[ "$rels" -gt 0 ]] || { echo "no relocations: -Wl,-q did not take" >&2; exit 1; }
  if vita-elf-create "$BUILD/probe.elf" "$BUILD/probe.velf" 2>"$BUILD/velf.err"; then
    built=1
    echo "==> linked (pad=$pad, $rels relocation sections)"
    break
  fi
  grep -q 'Cannot allocate' "$BUILD/velf.err" || { cat "$BUILD/velf.err" >&2; exit 1; }
  pad=$((pad + 512))
done
[[ "$built" -eq 1 ]] || { echo "could not pad the segment enough" >&2; exit 1; }

# Unsafe, to match how the real app is packaged. A packaging difference would
# be a difference between probe and app, which is exactly what must not vary.
vita-make-fself "$BUILD/probe.velf" "$BUILD/eboot.bin"

# This vitacompanion has no `install`, so the probe borrows the project's own
# registered slot. Back the real eboot up first and put it back afterwards.
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

# Every flag is set or cleared on every run: an inherited flag from a previous
# run would silently answer a different question than the one being asked.
: > "$BUILD/flag.txt"
for f in mem:GV_PROBE_MEM stack:GV_PROBE_STACK threads:GV_PROBE_THREADS display:GV_PROBE_DISPLAY smallstack:GV_PROBE_SMALLSTACK poll:GV_PROBE_POLL; do
  name="${f%%:*}"; var="${f##*:}"
  if [[ -n "${!var:-}" ]]; then
    put "$BUILD/flag.txt" "ux0:/data/galaxyvita/probe_$name.txt" >/dev/null
    echo "==> probe flag: $name"
  else
    del "ux0:/data/galaxyvita/probe_$name.txt"
  fi
done

del "ux0:/data/galaxyvita/lifetime.log"
put "$BUILD/eboot.bin" "ux0:/app/$TITLE/eboot.bin"
cmd "launch $TITLE" >/dev/null || true

echo "==> watching lifetime.log (expect 60 ticks)"
last=0
for _ in $(seq 1 26); do
  sleep 5
  ftp -o "$BUILD/lifetime.log" \
    "ftp://$IP:1337/ux0:/data/galaxyvita/lifetime.log" 2>/dev/null || true
  n=$(wc -l < "$BUILD/lifetime.log" 2>/dev/null || echo 0)
  if [[ "$n" -gt "$last" ]]; then
    tail -n +$(( last + 1 )) "$BUILD/lifetime.log" | sed 's/^/  /'
    last="$n"
  fi
  grep -q 'lifetime probe done' "$BUILD/lifetime.log" 2>/dev/null && break
done

echo
# Just the fact. What it means depends on which flags were set, and a verdict
# baked in here was wrong the first time the answer stopped being the expected
# one -- it called a display-flag survival evidence about GalaxyVita.
ticks=$(grep -c '^tick ' "$BUILD/lifetime.log" 2>/dev/null || echo 0)
if grep -q 'survived 60s' "$BUILD/lifetime.log" 2>/dev/null; then
  echo "RESULT: survived all 60 ticks with [${GV_PROBE_MEM:+mem }${GV_PROBE_STACK:+stack }${GV_PROBE_THREADS:+threads }${GV_PROBE_DISPLAY:+display }${GV_PROBE_SMALLSTACK:+smallstack }${GV_PROBE_POLL:+poll }]"
else
  echo "RESULT: stopped after $ticks ticks with [${GV_PROBE_MEM:+mem }${GV_PROBE_STACK:+stack }${GV_PROBE_THREADS:+threads }${GV_PROBE_DISPLAY:+display }${GV_PROBE_SMALLSTACK:+smallstack }${GV_PROBE_POLL:+poll }]"
fi
