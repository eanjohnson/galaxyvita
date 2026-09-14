#!/usr/bin/env bash
# Source this, or let other scripts source it.
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
export GALAXYVITA_ROOT="$ROOT"

if [[ -f "$ROOT/.env.local" ]]; then
  # shellcheck disable=SC1091
  source "$ROOT/.env.local"
fi
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
