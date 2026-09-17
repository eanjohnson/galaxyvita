#!/usr/bin/env bash
# Push the current build to a real Vita via vitacompanion. No game data is transferred.
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
# shellcheck disable=SC1091
source "$ROOT/scripts/vita-env.sh"

if [[ -z "${GALAXYVITA_IP:-}" ]]; then
  echo "Set GALAXYVITA_IP or write scripts/.vita-ip" >&2
  exit 1
fi

BUILD="$ROOT/generated/build/vita"
TITLE="${GALAXYVITA_TITLEID}"
IP="$GALAXYVITA_IP"

cmd() {
  # vitacompanion wants a newline. socat is present even when netcat is not.
  printf '%s\n' "$1" | socat - "TCP:$IP:1338,connect-timeout=5"
}

wake() {
  cmd "screen on; nosleep on" || true
}

if [[ "${1:-}" == "--vpk" ]]; then
  VPK=$(ls -1 "$BUILD"/*.vpk 2>/dev/null | head -1 || true)
  if [[ -z "$VPK" ]]; then
    echo "No VPK in $BUILD — run ./scripts/build-vita.sh" >&2
    exit 1
  fi
  wake
  echo "Uploading $(basename "$VPK") to ux0:/vpk/"
  curl -q --ftp-method nocwd -T "$VPK" "ftp://$IP:1337/ux0:/vpk/$(basename "$VPK")"
  echo "Install once with VitaShell (ux0:vpk/$(basename "$VPK")), then use ./scripts/deploy-vita.sh for eboot updates."
  exit 0
fi

EBOOT=""
for c in "$BUILD/eboot.bin" "$BUILD/galaxyvita.self" "$BUILD/hello_vita.self"; do
  if [[ -f "$c" ]]; then EBOOT="$c"; break; fi
done
# vita_create_self typically names it ${target}.self next to the elf
if [[ -z "$EBOOT" ]]; then
  EBOOT=$(ls -1 "$BUILD"/*.self 2>/dev/null | head -1 || true)
fi
if [[ -z "$EBOOT" ]]; then
  echo "No eboot/self in $BUILD — run ./scripts/build-vita.sh" >&2
  exit 1
fi

wake
# Stop our app by name, and only escalate if that did not take.
#
# `destroy` quits EVERY application, and on this console VitaShell is an
# application -- and VitaShell is what serves FTP on 1337. So the convenient
# hammer takes the file transport down with it: port 1337 stops answering while
# vitacompanion keeps replying on 1338, which reads as a wedged console and is
# really a self-inflicted one. It cost a reboot twice before the connection was
# made. `kill TITLEID` is less reliable, which is why this used to reach for
# `destroy` -- so try it, check, and fall back only when it actually failed.
stop_app() {
  cmd "kill $TITLE" >/dev/null 2>&1 || true
  sleep 2
  # If FTP still answers, nothing further is needed and nothing was risked.
  if curl -sS --connect-timeout 6 -l "ftp://$IP:1337/ux0:/app/$TITLE/" >/dev/null 2>&1; then
    return 0
  fi
  cmd "destroy" >/dev/null 2>&1 || true
  sleep 2
}
stop_app
echo "Uploading $(basename "$EBOOT") -> ux0:/app/$TITLE/eboot.bin"
curl -q --ftp-method nocwd -T "$EBOOT" "ftp://$IP:1337/ux0:/app/$TITLE/eboot.bin"
cmd "launch $TITLE"
echo "Launched $TITLE on $IP"
