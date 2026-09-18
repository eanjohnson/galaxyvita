#!/usr/bin/env bash
# Pull one frame off a GalaxyVita run that is already going.
#
#   ./scripts/snap-vita.sh                 -> generated/build/vita-run/snap.png
#   ./scripts/snap-vita.sh shots/menu.png
#
# The frame dump window is fixed when the backend comes up, which is no use for
# driving the game: the frame worth seeing is the one after a button press, and
# which frame that is cannot be known in advance. This writes a serial to
# snap.txt, the watchdog thread passes it to the renderer, and the next
# presented frame lands on the card.
#
# The download waits for the file to stop growing first. ftpvita cannot serve
# anything again after a RETR is cut short, and reading a 2 MB frame the render
# thread is still writing is exactly how to cut one short.
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
# shellcheck disable=SC1091
source "$ROOT/scripts/vita-env.sh"

[[ -n "${GALAXYVITA_IP:-}" ]] || { echo "Set GALAXYVITA_IP or write scripts/.vita-ip" >&2; exit 1; }
IP="$GALAXYVITA_IP"
DIR="ux0:/data/galaxyvita"
OUT="${1:-$ROOT/generated/build/vita-run/snap.png}"
W="${GALAXYVITA_SNAP_W:-960}"
H="${GALAXYVITA_SNAP_H:-544}"
WANT=$((W * H * 4))

exec 9>"${TMPDIR:-/tmp}/galaxyvita-device.lock"
flock -n 9 || { echo "another script is using the Vita; wait for it or kill it" >&2; exit 1; }

mkdir -p "$(dirname "$OUT")"
TMP="${TMPDIR:-/tmp}/gv-snap"
mkdir -p "$TMP"

# Delete first, so "the file is there" cannot be answered by the last one.
curl -sS --connect-timeout 10 -Q "-DELE $DIR/frames/snap.raw" "ftp://$IP:1337/" >/dev/null 2>&1 || true

SERIAL="$(date +%s)"
printf '%s\n' "$SERIAL" > "$TMP/snap.txt"
curl -sS --connect-timeout 10 -T "$TMP/snap.txt" "ftp://$IP:1337/$DIR/snap.txt"

size_of() {
  curl -sS --connect-timeout 10 -l "ftp://$IP:1337/$DIR/frames/" 2>/dev/null \
    | grep -qx 'snap.raw' || { echo 0; return; }
  curl -sS --connect-timeout 10 -I "ftp://$IP:1337/$DIR/frames/snap.raw" 2>/dev/null \
    | awk '/Content-Length/ {print $2+0}' | tr -d '\r'
}

for _ in $(seq 1 40); do
  sleep 3
  s="$(size_of)"
  [[ "${s:-0}" -ge "$WANT" ]] && break
done
[[ "${s:-0}" -ge "$WANT" ]] || { echo "no snapshot after 2 minutes (is the app running, and past gfx init?)" >&2; exit 1; }

curl -sS --connect-timeout 10 -o "$TMP/snap.raw" "ftp://$IP:1337/$DIR/frames/snap.raw"
python3 "$ROOT/scripts/raw_to_png.py" "$TMP/snap.raw" "$OUT" "$W" "$H"
echo "$OUT"
