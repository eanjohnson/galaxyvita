#!/usr/bin/env bash
# Recompile RMGE01 main.dol to portable C. Never writes Nintendo bytes into git.
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
# shellcheck disable=SC1091
source "$ROOT/scripts/vita-env.sh"

DOLRECOMP="${DOLRECOMP:-$ROOT/generated/build/dolrecomp/dolrecomp}"
DOL="${1:-$ROOT/generated/extracted/identity/sys/main.dol}"
OUT="${2:-$ROOT/generated/aot}"
TITLE="${GALAXYVITA_WII_TITLEID:-RMGE01}"
PIN="2c680585a8f58e1cc9c5521b579057f12b124ff0ef409e470a57606c50a93c09"

if [[ ! -x "$DOLRECOMP" ]]; then
  echo "dolrecomp missing at $DOLRECOMP — build third_party/DolRecomp first" >&2
  exit 1
fi
if [[ ! -f "$DOL" ]]; then
  echo "DOL missing at $DOL — run ./scripts/extract-disc.sh" >&2
  exit 1
fi

got=$(sha256sum "$DOL" | awk '{print $1}')
if [[ "$got" != "$PIN" ]]; then
  echo "refusing to recompile: $DOL sha256 $got != GalaxyPad/RMGE01 pin $PIN" >&2
  exit 1
fi

mkdir -p "$OUT"
JOBS="${DOLRECOMP_JOBS:-$(nproc)}"
# GalaxyPad used 1024-instruction C chunks. Current DolRecomp default is 4096;
# keep 1024 so the Vita size number is comparable to that process.
export DOLRECOMP_C_CHUNK_INSTRUCTIONS="${DOLRECOMP_C_CHUNK_INSTRUCTIONS:-1024}"

echo "==> dolrecomp $DOL $TITLE -> $OUT  (-j$JOBS chunk=$DOLRECOMP_C_CHUNK_INSTRUCTIONS)"
"$DOLRECOMP" -j"$JOBS" --cpu broadway --backend c "$DOL" "$TITLE" "$OUT"
echo "generated under $OUT"
# Local `bl`s to HLE'd functions compile to plain gotos; make them escape to the
# chassis dispatcher so runtime/hle_hooks.h actually takes effect.
python3 "$ROOT/scripts/patch-aot-hle.py" "$OUT/generated/chunks" "$ROOT/runtime/hle_hooks.h"
# pipefail + head is SIGPIPE (141); list a sample without failing the script
mapfile -t sample < <(find "$OUT" -type f | sed "s|^$ROOT/||" | sort)
printf '%s\n' "${sample[@]:0:12}"
echo "... ${#sample[@]} files, $(du -sh "$OUT" | awk '{print $1}')"
