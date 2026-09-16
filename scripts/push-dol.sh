#!/usr/bin/env bash
# Copy the extracted RMGE01 DOL to the Vita. Never packaged into the VPK.
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
# shellcheck disable=SC1091
source "$ROOT/scripts/vita-env.sh"
DOL="${1:-$ROOT/generated/extracted/identity/sys/main.dol}"
PIN="2c680585a8f58e1cc9c5521b579057f12b124ff0ef409e470a57606c50a93c09"
if [[ ! -f "$DOL" ]]; then
  echo "DOL missing — run ./scripts/extract-disc.sh" >&2
  exit 1
fi
got=$(sha256sum "$DOL" | awk '{print $1}')
if [[ "$got" != "$PIN" ]]; then
  echo "refusing to push DOL: hash $got != pin $PIN" >&2
  exit 1
fi
cmd() { printf '%s\n' "$1" | socat - "TCP:$GALAXYVITA_IP:1338,connect-timeout=5"; }
cmd "screen on; nosleep on" || true
curl -s --connect-timeout 8 --ftp-method nocwd "ftp://$GALAXYVITA_IP:1337/ux0:/data/" \
  --quote "MKD ux0:/data/galaxyvita" >/dev/null || true
curl -s --connect-timeout 8 --ftp-method nocwd "ftp://$GALAXYVITA_IP:1337/ux0:/data/galaxyvita/" \
  --quote "MKD ux0:/data/galaxyvita/sys" >/dev/null || true
echo "Uploading $(basename "$DOL") -> ux0:/data/galaxyvita/sys/main.dol"
curl --connect-timeout 30 --ftp-method nocwd -T "$DOL" \
  "ftp://$GALAXYVITA_IP:1337/ux0:/data/galaxyvita/sys/main.dol"
echo "pushed DOL ($got)"
