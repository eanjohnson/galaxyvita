#!/usr/bin/env bash
# Source this, or let other scripts source it.
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
export GALAXYVITA_ROOT="$ROOT"

_exported_ip="${GALAXYVITA_IP:-}"
if [[ -f "$ROOT/.env.local" ]]; then
  # shellcheck disable=SC1091
  source "$ROOT/.env.local"
fi
# The console moves between LANs, so .env.local lists every address it has
# (GALAXYVITA_IPS) and the first one whose vitacompanion answers is used. An
# exported GALAXYVITA_IP wins; with nothing answering, the first is assumed.
if [[ -n "$_exported_ip" ]]; then
  GALAXYVITA_IP="$_exported_ip"
elif [[ -n "${GALAXYVITA_IPS:-}" ]]; then
  GALAXYVITA_IP=""
  for _ip in $GALAXYVITA_IPS; do
    if timeout 2 bash -c "exec 3<>/dev/tcp/$_ip/1338" 2>/dev/null; then
      GALAXYVITA_IP="$_ip"
      break
    fi
  done
  if [[ -z "$GALAXYVITA_IP" ]]; then
    read -r GALAXYVITA_IP _ <<< "$GALAXYVITA_IPS"
  fi
fi
unset _exported_ip _ip
[[ -n "${GALAXYVITA_IP:-}" ]] && export GALAXYVITA_IP
if [[ -f "$ROOT/scripts/.vita-ip" ]]; then
  GALAXYVITA_IP="$(tr -d '[:space:]' < "$ROOT/scripts/.vita-ip")"
  export GALAXYVITA_IP
fi

export GALAXYVITA_TITLEID="${GALAXYVITA_TITLEID:-GVITA0001}"
export GALAXYVITA_APP_NAME="${GALAXYVITA_APP_NAME:-GalaxyVita}"
export GALAXYVITA_VERSION="${GALAXYVITA_VERSION:-00.01}"

if [[ -z "${VITASDK:-}" ]]; then
  if [[ -x "$HOME/vitasdk/bin/arm-vita-eabi-gcc" ]]; then
    export VITASDK="$HOME/vitasdk"
  elif [[ -x /usr/local/vitasdk/bin/arm-vita-eabi-gcc ]]; then
    export VITASDK=/usr/local/vitasdk
  fi
fi
if [[ -n "${VITASDK:-}" ]]; then
  export PATH="$VITASDK/bin:$PATH"
fi
