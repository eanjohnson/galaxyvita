#!/usr/bin/env bash
# Install VitaSDK into $HOME/vitasdk and host-side Node CLI. Never fetches game data.
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
# shellcheck disable=SC1091
source "$ROOT/scripts/vita-env.sh"

VITASDK_PREFIX="${VITASDK_PREFIX:-$HOME/vitasdk}"
VDPM_DIR="$ROOT/extern/vdpm"

echo "==> VitaSDK prefix: $VITASDK_PREFIX"

if [[ ! -x "$VITASDK_PREFIX/bin/arm-vita-eabi-gcc" ]]; then
  mkdir -p "$ROOT/extern"
  if [[ ! -d "$VDPM_DIR/.git" ]]; then
    git clone --depth 1 https://github.com/vitasdk/vdpm.git "$VDPM_DIR"
  fi
  export VITASDK="$VITASDK_PREFIX"
  # vdpm bootstrap expects to write $VITASDK; create the parent.
  mkdir -p "$(dirname "$VITASDK_PREFIX")"
  (
    cd "$VDPM_DIR"
    ./bootstrap-vitasdk.sh
  )
else
  echo "VitaSDK already present at $VITASDK_PREFIX"
fi

export VITASDK="$VITASDK_PREFIX"
export PATH="$VITASDK/bin:$PATH"

if [[ -x "$VITASDK/bin/vdpm" ]]; then
  echo "==> Installing common vdpm packages (zlib, vitaGL if catalogued)"
  # Best-effort; catalog names vary by snapshot.
  vdpm install zlib || true
  vdpm install vitagl || vdpm install vitaGL || true
fi

echo "==> vitacompanion-cli (local prefix, no root)"
mkdir -p "$HOME/.local"
npm install -g --prefix "$HOME/.local" vitacompanion-cli

echo "==> Checking toolchain"
arm-vita-eabi-gcc -v
command -v vita || echo "vita (vitacompanion-cli) not on PATH; add $HOME/.local/bin"

echo "Add to your shell:"
echo "  export VITASDK=$VITASDK_PREFIX"
echo "  export PATH=\"\$VITASDK/bin:\$HOME/.local/bin:\$PATH\""
echo "  export GALAXYVITA_IP=<vita-lan-ip>"
