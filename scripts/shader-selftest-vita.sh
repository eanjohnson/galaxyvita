#!/usr/bin/env bash
# Answers the one graphics question the host cannot: does the Vita's own Cg
# compiler accept what Dolphin generates?
#
# Builds a standalone self-test VPK, stages the translated Cg next to it, runs
# it over vitacompanion and fetches the report. No interaction on the console.
#
#   ./scripts/run-host-chassis.sh ... GV_GFX_STATEDUMP=<states>
#   ./scripts/dump-dolphin-shaders.sh <states> <glsl>
#   ./scripts/shader-selftest-vita.sh <glsl>
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
GLSL="${1:-$ROOT/generated/build/dolphin-shaders}"
CG="$ROOT/generated/build/vita-cg"
BUILD="$ROOT/generated/build/shader-selftest"
TITLE="${GALAXYVITA_TITLEID:-GVITA0001}"
# shellcheck disable=SC1091
source "$ROOT/scripts/vita-env.sh"
: "${VITASDK:=$HOME/vitasdk}"
export VITASDK PATH="$VITASDK/bin:$PATH"

# Only one client may talk to the console at a time. Two concurrent runs wedge
# vitacompanion's FTP and each one's restore trap fights the other's staging --
# which is how the app slot got left holding a self-test binary. Fail loudly
# rather than queue: a second run is always a mistake, never a wait.
exec 9>"${TMPDIR:-/tmp}/galaxyvita-device.lock"
flock -n 9 || { echo "another script is using the Vita; wait for it or kill it" >&2; exit 1; }

[[ -n "${GALAXYVITA_IP:-}" ]] || { echo "Set GALAXYVITA_IP or write scripts/.vita-ip" >&2; exit 1; }
IP="$GALAXYVITA_IP"
cmd() { printf '%s\n' "$1" | socat - "TCP:$IP:1338,connect-timeout=5"; }
# Stop our app by name rather than quitting every application.
#
# `destroy` also kills VitaShell, which is what serves FTP on 1337 -- so the
# convenient hammer takes the file transport down with it, and the console
# looks wedged when it is only missing its FTP server. Escalate only if the
# targeted kill did not take.
stop_app() {
  cmd "kill $TITLE" >/dev/null 2>&1 || true
  sleep 2
  if curl -sS --connect-timeout 6 -l "ftp://$IP:1337/ux0:/" >/dev/null 2>&1; then
    return 0
  fi
  cmd "destroy" >/dev/null 2>&1 || true
  sleep 2
}


# GV_CG_DIR stages a prepared directory of .cg verbatim instead of translating.
# Used to bisect a compiler hang down to a single construct without waiting on
# a full 64-shader run.
mkdir -p "$BUILD"
if [[ -n "${GV_CG_DIR:-}" ]]; then
  CG="$GV_CG_DIR"
  echo "==> staging prepared Cg from $CG ($(find "$CG" -name '*.cg' | wc -l) files)"
else
  rm -rf "$CG"; mkdir -p "$CG"
  n=0
  for f in "$GLSL"/*.frag "$GLSL"/*.vert; do
    [[ -e "$f" ]] || continue
    python3 "$ROOT/scripts/glsl_to_cg.py" "$f" "$CG/$(basename "$f").cg"
    n=$((n + 1))
  done
  echo "==> translated $n shaders to Cg"
fi

# -Wl,-q is mandatory: vita-elf-create needs the relocations to build a
# loadable module, and it accepts an ELF without them silently. The result
# launches, jumps into padding, and dies before main with no diagnostic.
arm-vita-eabi-gcc -O2 -Wall -Wextra -Wl,-q -o "$BUILD/selftest.elf" \
  "$ROOT/vita/gfx/shader_selftest.c" \
  -lvitaGL -lmathneon -lvitashark -lSceShaccCgExt -lSceShaccCg_stub \
  -ltaihen_stub -lSceGxm_stub -lSceDisplay_stub -lSceCtrl_stub \
  -lSceCommonDialog_stub -lSceSysmodule_stub -lSceAppMgr_stub \
  -lSceKernelDmacMgr_stub -lSceIofilemgr_stub -lSceSysmem_stub \
  -lSceLibKernel_stub -lSceKernelThreadMgr_stub -lstdc++ -lm
vita-elf-create "$BUILD/selftest.elf" "$BUILD/selftest.velf"
# Unsafe: ShaccCg returns "fatal internal error" for every program, including a
# two-line control shader, when the app is a safe fself -- the module loads
# (shark_init returns 0) but the compiler cannot do its work.
vita-make-fself "$BUILD/selftest.velf" "$BUILD/eboot.bin"

ftp() { curl -sS --connect-timeout 10 "$@"; }
put() { ftp --ftp-create-dirs -T "$1" "ftp://$IP:1337/$2"; }

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
  stop_app
  # The app can still hold its own eboot for a moment after destroy, which
  # comes back as FTP 550. Retry rather than leave the console holding the
  # self-test binary in the project's app slot.
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
# Stale .cg from a previous run would be compiled too -- the self-test walks
# whatever the directory holds -- so clear it before staging.
for old in $(ftp --list-only "ftp://$IP:1337/ux0:/data/galaxyvita/shaders/" 2>/dev/null); do
  ftp -Q "-DELE ux0:/data/galaxyvita/shaders/$old" "ftp://$IP:1337/ux0:/data/galaxyvita/" >/dev/null 2>&1 || true
done
for f in "$CG"/*.cg; do put "$f" "ux0:/data/galaxyvita/shaders/$(basename "$f")"; done
stop_app
put "$BUILD/eboot.bin" "ux0:/app/$TITLE/eboot.bin"

# ShaccCg wedges after a handful of compiles in one process, so the self-test
# compiles a bounded batch and records where it stopped. Relaunch until it
# reports a summary; a fresh process reclaims whatever the compiler leaked.
ftp -Q "-DELE ux0:/data/galaxyvita/selftest_progress.txt" \
    "ftp://$IP:1337/ux0:/data/galaxyvita/" >/dev/null 2>&1 || true

total_cg=$(find "$CG" -name '*.cg' | wc -l)
launches=$(( total_cg + 4 ))
del() { ftp -Q "-DELE $1" "ftp://$IP:1337/ux0:/data/galaxyvita/" >/dev/null 2>&1 || true; }

for launch in $(seq 1 "$launches"); do
  # Clear the marker first: its reappearance is the only reliable signal that
  # this launch ran, as opposed to the previous launch's report still on disk.
  del "ux0:/data/galaxyvita/selftest_batch.txt"
  stop_app
  sleep 2
  cmd "launch $TITLE" >/dev/null || true

  got=0
  for _ in $(seq 1 40); do
    sleep 10
    if ftp -o "$BUILD/batch.txt" \
         "ftp://$IP:1337/ux0:/data/galaxyvita/selftest_batch.txt" 2>/dev/null; then
      got=1
      break
    fi
  done

  if [[ "$got" -eq 0 ]]; then
    # The batch wedged instead of finishing. Progress is recorded per shader,
    # so the next launch resumes past whatever hung.
    echo "==> launch $launch wedged; resuming"
    continue
  fi

  read -r done_n total_n < "$BUILD/batch.txt"
  echo "==> launch $launch: $done_n of $total_n"
  if [[ "$done_n" -ge "$total_n" ]]; then
    ftp -o "$BUILD/report.txt" \
      "ftp://$IP:1337/ux0:/data/galaxyvita/shader_selftest.txt" 2>/dev/null || true
    cat "$BUILD/report.txt"
    exit 0
  fi
done
echo "ran out of launches; last state:" >&2
cat "$BUILD/report.txt" 2>/dev/null >&2 || echo "  no report written" >&2
exit 1
