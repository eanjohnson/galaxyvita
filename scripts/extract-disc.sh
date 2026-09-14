#!/usr/bin/env bash
# Extract identity files or the full DATA partition. Never writes into git.
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
IMG="${1:-}"
MODE="${2:-identity}"  # identity | full

if [[ -z "$IMG" ]]; then
  shopt -s nullglob
  c=("$ROOT"/*.wbfs)
  shopt -u nullglob
  [[ ${#c[@]} -eq 1 ]] || { echo "usage: $0 <image.wbfs> [identity|full]" >&2; exit 1; }
  IMG="${c[0]}"
fi

WIT=""
for c in "$ROOT/extern/wit/"*/bin/wit "$ROOT/extern/wit/bin/wit"; do
  [[ -x "$c" ]] && WIT="$c" && break
done
if [[ -z "$WIT" ]]; then
  echo "wit not found under extern/wit. Download WIT v3.05a x86_64 from https://wit.wiimm.de/download.html" >&2
  exit 1
fi

DOLRECOMP="$ROOT/generated/build/dolrecomp/dolrecomp"
OUT="$ROOT/generated/extracted/${MODE}"
mkdir -p "$OUT"

case "$MODE" in
  identity)
    "$WIT" EXTRACT --dest "$OUT" --files '+/sys/main.dol' --overwrite "$IMG"
    ;;
  full)
    if [[ -x "$DOLRECOMP" ]]; then
      "$DOLRECOMP" extract --wit "$WIT" "$IMG" "$OUT"
    else
      "$WIT" EXTRACT --dest "$OUT" --overwrite "$IMG"
    fi
    ;;
  *)
    echo "mode must be identity or full" >&2
    exit 1
    ;;
esac

echo "extracted to $OUT"
