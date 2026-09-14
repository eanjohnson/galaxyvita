#!/usr/bin/env bash
# Fetch the plugins listed in docs/VITA-SETUP.md into extern/vita-plugins/.
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
DEST="$ROOT/extern/vita-plugins"
mkdir -p "$DEST"

dl() {
  local dest="$DEST/$1" url="$2"
  mkdir -p "$(dirname "$dest")"
  echo "GET $url"
  curl -fL --retry 3 --retry-delay 1 -o "$dest" "$url"
}

dl vitacompanion/vitacompanion.suprx \
  https://github.com/devnoname120/vitacompanion/releases/download/1.06/vitacompanion.suprx
dl vitacompanion/vitacompanion_kernel.skprx \
  https://github.com/devnoname120/vitacompanion/releases/download/1.06/vitacompanion_kernel.skprx

dl princesslog/net_logging_mgr.skprx \
  https://github.com/isage/plog/releases/download/v1.0/net_logging_mgr.skprx
dl princesslog/NetLoggingMgrSettings.vpk \
  https://github.com/isage/plog/releases/download/v1.0/NetLoggingMgrSettings.vpk
dl catlog/catlog.skprx \
  https://github.com/isage/catlog/releases/download/v1.2/catlog.skprx
dl catlog/catlog.suprx \
  https://github.com/isage/catlog/releases/download/v1.2/catlog.suprx

dl capunlocker/CapUnlocker.skprx \
  https://github.com/GrapheneCt/CapUnlocker/releases/download/v1.4/CapUnlocker.skprx
dl lowmemmode/LowMemMode.suprx \
  https://github.com/GrapheneCt/LowMemMode/releases/download/v1.2/LowMemMode.suprx

dl psvshell/PSVshell.skprx \
  https://github.com/Electry/PSVshell/releases/download/v1.2beta/PSVshell.skprx
dl psvshellplus/PSVshellPlus_Kernel.skprx \
  https://github.com/GrapheneCt/PSVshellPlus/releases/download/v1.4/PSVshellPlus_Kernel.skprx
dl psvshellplus/PSVshellPlus_Shell.suprx \
  https://github.com/GrapheneCt/PSVshellPlus/releases/download/v1.4/PSVshellPlus_Shell.suprx
dl psvshellplus/psvshell_plugin.rco \
  https://github.com/GrapheneCt/PSVshellPlus/releases/download/v1.4/psvshell_plugin.rco

dl oclockvita/oclockvita.suprx \
  https://github.com/frangarcj/oclockvita/releases/download/1.2.1/oclockvita.suprx

dl sharkbr33d/ShaRKBR33D.vpk \
  https://github.com/Rinnegatamante/ShaRKBR33D/releases/download/v.1.2/ShaRKBR33D.vpk

echo
find "$DEST" -type f ! -name 'MANIFEST.md' | sort | xargs -r sha256sum
echo "Wrote $DEST"
