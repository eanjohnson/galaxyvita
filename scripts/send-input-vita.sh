#!/usr/bin/env bash
# Drive the Wii Remote of a GalaxyVita run that is already going.
#
# Nothing else on this console can. vitacompanion's `press` answers "Input
# pressed." and the input reaches neither SceShell nor the application, and
# vita_pad reads through the same sceCtrlPeekBufferPositive that it fails to
# reach -- so synthetic input has to arrive as data the app reads for itself.
# This writes ux0:data/galaxyvita/input.txt, which the watchdog thread polls
# every two seconds and the pad plays back by retrace frame.
#
# Steps are <keys>[:frames], played once in order:
#
#   ./scripts/send-input-vita.sh a                 press A
#   ./scripts/send-input-vita.sh a:30 wait:60 a    press, pause, press again
#   ./scripts/send-input-vita.sh up+a              two buttons at once
#   ./scripts/send-input-vita.sh --ir 512,384 a    park the pointer first
#
# --ir takes the pointer in the Wii IR camera's own units, 0..1023 by 0..767,
# and vita_pad mirrors x on the way in because the camera faces the player (see
# the comment there). After that mirror, measured on the file-select screen
# against the 960x544 display:
#
#   ir_x ~= 0.590 * screen_x + 225       512 is centre; larger x goes right
#   ir_y ~= 0.833 * screen_y + 224       384 is centre; below ~259 the game
#                                        clamps the cursor to screen y=42
#
# So the centre of the screen is 512,384, and ir_x 300..723 covers screen x
# 128..845; outside that the cursor clamps to the edge. The y scale is not the
# x scale; do not assume one calibration covers both.
#
# Measure these by reading the watchdog's "ir script=.. sent=.." line together
# with the cursor, on one boot. Fitting cursor positions alone produced two
# wrong answers: a slope through two clamped endpoints, and a sweep on a
# different boot that came out with the opposite sign for reasons still not
# understood. If a pointer result surprises you, check it against that line
# before believing it, and do not compare sweeps taken across a restart.
#
# Frames are the video frame, 60 to the second. A release gap goes in between
# consecutive presses automatically, because a button the game never sees
# released is not a press.
#
#   --periodic <keys> [period] [hold]   write btn.txt instead: hold <keys> for
#                                       <hold> frames out of every <period>,
#                                       forever. The way past a screen that
#                                       only wants any button at all.
#   --clear                             stop both.
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
# shellcheck disable=SC1091
source "$ROOT/scripts/vita-env.sh"

[[ -n "${GALAXYVITA_IP:-}" ]] || { echo "Set GALAXYVITA_IP or write scripts/.vita-ip" >&2; exit 1; }
IP="$GALAXYVITA_IP"
DIR="ux0:/data/galaxyvita"

# One client at a time, like every other device script.
exec 9>"${TMPDIR:-/tmp}/galaxyvita-device.lock"
flock -n 9 || { echo "another script is using the Vita; wait for it or kill it" >&2; exit 1; }

# The masks are the Wii Remote's own bit order (runtime/bt.h). The nunchuk
# rides in bits 16 and up of the same field so that a step is always exactly
# two numbers and the device-side parse cannot go out of step.
key_mask() {
  local out=0 k
  IFS='+' read -ra parts <<< "$1"
  for k in "${parts[@]}"; do
    case "$k" in
      left)  out=$((out | 0x0001)) ;;
      right) out=$((out | 0x0002)) ;;
      down)  out=$((out | 0x0004)) ;;
      up)    out=$((out | 0x0008)) ;;
      plus)  out=$((out | 0x0010)) ;;
      two)   out=$((out | 0x0100)) ;;
      one)   out=$((out | 0x0200)) ;;
      b)     out=$((out | 0x0400)) ;;
      a)     out=$((out | 0x0800)) ;;
      minus) out=$((out | 0x1000)) ;;
      home)  out=$((out | 0x8000)) ;;
      z)     out=$((out | 0x010000)) ;;
      c)     out=$((out | 0x020000)) ;;
      wait|none|idle) ;;
      *) echo "unknown key '$k' (a b one two plus minus home up down left right z c wait)" >&2; exit 1 ;;
    esac
  done
  printf '%x' "$out"
}

IR=""
MODE=steps
case "${1:-}" in
  --periodic)
    shift
    [[ $# -ge 1 ]] || { echo "--periodic needs keys" >&2; exit 1; }
    MASK="$(key_mask "$1")"
    PERIOD="${2:-90}"
    HOLD="${3:-30}"
    printf '%s %s %s\n' "$MASK" "$PERIOD" "$HOLD" > "${TMPDIR:-/tmp}/gv-btn.txt"
    curl -sS --connect-timeout 10 -T "${TMPDIR:-/tmp}/gv-btn.txt" "ftp://$IP:1337/$DIR/btn.txt"
    echo "btn.txt: mask=0x$MASK period=$PERIOD hold=$HOLD (applies within 2s)"
    exit 0 ;;
  --clear)
    printf '0 90 30\n' > "${TMPDIR:-/tmp}/gv-btn.txt"
    curl -sS --connect-timeout 10 -T "${TMPDIR:-/tmp}/gv-btn.txt" "ftp://$IP:1337/$DIR/btn.txt"
    MODE=clear ;;
  --ir)
    shift
    IR="${1:-}"; shift ;;
esac

# The header is always serial + pointer, three numbers; -1 leaves the pad's
# own pointer alone. Optional fields here read as the first step of the script.
SERIAL="$(date +%s)"
OUT="${TMPDIR:-/tmp}/gv-input.txt"
if [[ -n "$IR" ]]; then
  printf '%s %s %s\n' "$SERIAL" "${IR%,*}" "${IR#*,}" > "$OUT"
else
  printf '%s -1 -1\n' "$SERIAL" > "$OUT"
fi

if [[ "$MODE" == clear ]]; then
  printf '0 1\n' >> "$OUT"
  DESC="cleared"
else
  [[ $# -ge 1 ]] || { echo "nothing to send; try: $0 a" >&2; exit 1; }
  DESC=""
  first=1
  for step in "$@"; do
    keys="${step%%:*}"
    frames="${step#*:}"
    [[ "$frames" == "$step" ]] && frames=20
    mask="$(key_mask "$keys")"
    # A release between consecutive presses, so the game sees an edge.
    if [[ $first -eq 0 && "$mask" != 0 ]]; then
      printf '0 12\n' >> "$OUT"
    fi
    printf '%s %s\n' "$mask" "$frames" >> "$OUT"
    DESC="$DESC $keys:$frames"
    first=0
  done
fi

curl -sS --connect-timeout 10 -T "$OUT" "ftp://$IP:1337/$DIR/input.txt"
echo "input.txt serial=$SERIAL:$DESC${IR:+ ir=$IR} (starts within 2s)"
