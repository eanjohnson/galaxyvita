#!/usr/bin/env bash
# Deploy the current build, run it on the console, and bring its logs back.
#
# This is the whole device loop. Nothing here needs anyone at the Vita: it
# uploads the eboot, launches the title, waits for the boot to reach its own
# "GalaxyVita phase4 exit" line, and fetches boot.log and watchdog.log.
#
# The watchdog log is the one that distinguishes a hung boot from a slow one.
# A missing line, a frozen dispatch count and a rising one are three different
# diagnoses, so it is fetched even when the run never finishes.
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
# shellcheck disable=SC1091
source "$ROOT/scripts/vita-env.sh"

BUILD="$ROOT/generated/build/vita"
OUT="$ROOT/generated/build/vita-run"
WAIT="${GALAXYVITA_RUN_WAIT:-300}"

# One client at a time. Two concurrent runs wedge vitacompanion's FTP and cost
# a power cycle.
exec 9>"${TMPDIR:-/tmp}/galaxyvita-device.lock"
flock -n 9 || { echo "another script is using the Vita; wait for it or kill it" >&2; exit 1; }

[[ -n "${GALAXYVITA_IP:-}" ]] || { echo "Set GALAXYVITA_IP or write scripts/.vita-ip" >&2; exit 1; }
IP="$GALAXYVITA_IP"
TITLE="${GALAXYVITA_TITLEID}"
[[ -f "$BUILD/eboot.bin" ]] || { echo "no eboot.bin — run ./scripts/build-vita.sh" >&2; exit 1; }

mkdir -p "$OUT"
cmd() { printf '%s\n' "$1" | socat - "TCP:$IP:1338,connect-timeout=5"; }
ftp() { curl -sS --connect-timeout 10 "$@"; }
del() { ftp -Q "-DELE $1" "ftp://$IP:1337/" >/dev/null 2>&1 || true; }

cmd "screen on" >/dev/null || true
cmd "nosleep on" >/dev/null || true
cmd "destroy" >/dev/null || true

# The app truncates boot.log at startup, but deleting both first means a stale
# report from the previous launch can never be mistaken for this one's.
del "ux0:/data/galaxyvita/boot.log"
del "ux0:/data/galaxyvita/watchdog.log"

# GV_NOGFX=1 boots with the GXM backend off, which is how a stall gets pinned
# on the graphics side or ruled off it without building a second binary. The
# flag is always set or always cleared here, never left over: a stale one would
# silently disable graphics on a run meant to exercise them.
if [[ -n "${GV_NOGFX:-}" ]]; then
  : > "$OUT/nogfx.txt"
  ftp --ftp-method nocwd -T "$OUT/nogfx.txt" \
    "ftp://$IP:1337/ux0:/data/galaxyvita/nogfx.txt" >/dev/null
  echo "==> graphics DISABLED for this run (nogfx.txt)"
else
  del "ux0:/data/galaxyvita/nogfx.txt"
fi

# GV_IDLE=1 sets everything up and emulates nothing, which is how "the app was
# stopped from outside" gets told apart from "the chassis did something fatal".
if [[ -n "${GV_IDLE:-}" ]]; then
  : > "$OUT/idle.txt"
  ftp --ftp-method nocwd -T "$OUT/idle.txt" \
    "ftp://$IP:1337/ux0:/data/galaxyvita/idle.txt" >/dev/null
  echo "==> IDLE run: no emulation, watchdog only"
else
  del "ux0:/data/galaxyvita/idle.txt"
fi

echo "==> uploading eboot.bin to $IP"
ftp --ftp-method nocwd -T "$BUILD/eboot.bin" "ftp://$IP:1337/ux0:/app/$TITLE/eboot.bin"
cmd "launch $TITLE" >/dev/null || true

echo "==> waiting up to ${WAIT}s for the boot to finish"
# Print the watchdog as it arrives rather than only at the end. A run that is
# going to fail usually says so in the dispatch rate long before the timeout,
# and watching it live is the difference between one round-trip and three.
done_=0
seen=0
for _ in $(seq 1 $(( WAIT / 5 )) ); do
  sleep 5
  # Re-assert on every poll. "nosleep on" sent before the launch applies to
  # whatever was running then, not to the app we are about to start, and an
  # app the system suspends stops every one of its threads at once -- which
  # reads from here exactly like a hang, right down to leaving the process
  # resident with no core dump.
  cmd "nosleep on" >/dev/null 2>&1 || true
  if ftp -o "$OUT/watchdog.log" \
       "ftp://$IP:1337/ux0:/data/galaxyvita/watchdog.log" 2>/dev/null; then
    lines=$(wc -l < "$OUT/watchdog.log")
    if [[ "$lines" -gt "$seen" ]]; then
      tail -n +$(( seen + 1 )) "$OUT/watchdog.log" | sed 's/^/  /'
      seen=$lines
    fi
  fi
  if ftp -o "$OUT/boot.log" "ftp://$IP:1337/ux0:/data/galaxyvita/boot.log" 2>/dev/null &&
     grep -q 'phase4 exit' "$OUT/boot.log"; then
    done_=1
    break
  fi
done

echo "==> boot.log -> $OUT/boot.log"
if [[ "$done_" -eq 1 ]]; then
  tail -"${GALAXYVITA_RUN_TAIL:-20}" "$OUT/boot.log"
else
  tail -"${GALAXYVITA_RUN_TAIL:-20}" "$OUT/boot.log" 2>/dev/null || true
  echo "the boot did not reach 'phase4 exit' within ${WAIT}s" >&2
  exit 1
fi
