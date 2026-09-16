#!/usr/bin/env bash
# List DOL/REL/RSO/SEL paths from the WBFS. Does not extract game bytes into git.
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
IMG="${1:-}"
if [[ -z "$IMG" ]]; then
  shopt -s nullglob
  c=("$ROOT"/*.wbfs)
  shopt -u nullglob
  [[ ${#c[@]} -eq 1 ]] || { echo "usage: $0 <image.wbfs>" >&2; exit 1; }
  IMG="${c[0]}"
fi

WIT=""
for c in "$ROOT/extern/wit/"*/bin/wit "$ROOT/extern/wit/bin/wit"; do
  [[ -x "$c" ]] && WIT="$c" && break
done
[[ -n "$WIT" ]] || { echo "wit not found under extern/wit" >&2; exit 1; }

OUT="$ROOT/generated/extracted/inventory"
mkdir -p "$OUT"
LIST="$OUT/modules.txt"

# Files listing is metadata (paths/sizes), not file contents.
"$WIT" FILES --no-header "$IMG" | awk '
  BEGIN { IGNORECASE=1 }
  $NF ~ /\.(dol|rel|rso|sel)$/ { print }
' > "$LIST"

echo "==> executable-class files (path + size from wit FILES)"
cat "$LIST"
echo
echo "counts:"
awk '
  BEGIN { IGNORECASE=1 }
  $NF ~ /\.dol$/ { d++ }
  $NF ~ /\.rel$/ { r++ }
  $NF ~ /\.rso$/ { o++ }
  $NF ~ /\.sel$/ { s++ }
  END { printf "  dol=%d rel=%d rso=%d sel=%d\n", d+0, r+0, o+0, s+0 }
' "$LIST"
echo "wrote $LIST"
