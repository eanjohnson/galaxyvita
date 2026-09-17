#!/usr/bin/env bash
# Build gvlaunch.suprx: a second SceShell plugin that offers launch paths the
# stock vitacompanion does not.
#
# vitacompanion's `launch` is sceAppMgrLaunchAppByUri(0x20000, "psgm:play?..."),
# and an app launched that way is killed ~14 s later, while the same binary
# started from its LiveArea bubble runs as long as it likes. This exists to
# test other ways in. It is a fork of upstream vitacompanion built to *coexist*
# with the installed one rather than replace it:
#
#   - command port 1339, so the stock server keeps 1338
#   - module name gvlaunch, so taiHEN sees a different module
#   - GV_MINIMAL: command server only -- no FTP, no input thread, no nosleep,
#     no taipool. The less it does inside SceShell the less it can break, and
#     if it fails to load the stock plugin is untouched and the console stays
#     reachable. It is listed last in ur0:tai/config.txt for the same reason.
#
# Install: upload build/gvlaunch.suprx to ur0:tai/gvlaunch.suprx and add that
# path as the last line of the *main section. Remove the line to revert.
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/../.." && pwd)"
WORK="${GVLAUNCH_WORK:-$ROOT/generated/build/gvlaunch}"
UPSTREAM=https://github.com/devnoname120/vitacompanion
COMMIT="$(cat "$ROOT/vita/plugin/upstream-commit.txt")"

source "$ROOT/scripts/vita-env.sh"
: "${VITASDK:=$HOME/vitasdk}"
export VITASDK PATH="$VITASDK/bin:$PATH"

# libk is where strcmp/strcpy come from for this plugin, and it is not part of
# vitasdk-core. taipool and ftpvita are only needed for the FTP server, which
# the minimal build does not have.
[[ -f "$VITASDK/arm-vita-eabi/lib/libk.a" ]] || { echo "==> installing libk"; yes | vdpm libk; }

if [[ ! -d "$WORK/src" ]]; then
  echo "==> cloning upstream vitacompanion"
  rm -rf "$WORK"
  mkdir -p "$(dirname "$WORK")"
  git clone -q "$UPSTREAM" "$WORK"
fi
git -C "$WORK" fetch -q origin "$COMMIT" 2>/dev/null || true
git -C "$WORK" checkout -q "$COMMIT"
git -C "$WORK" checkout -q -- .
git -C "$WORK" apply "$ROOT/vita/plugin/gvlaunch.patch"

cmake -S "$WORK" -B "$WORK/build" \
  -DVITACOMPANION_CMD_PORT=1339 \
  -DVITACOMPANION_FTP_PORT=1341 \
  -DVITACOMPANION_MODULE_NAME=gvlaunch \
  -DGV_MINIMAL=1 -DGV_LAUNCH_FLAGS=0x20000 >/dev/null
cmake --build "$WORK/build" -j"$(nproc)" >/dev/null
echo "==> $WORK/build/gvlaunch.suprx"
ls -la "$WORK/build/gvlaunch.suprx"
