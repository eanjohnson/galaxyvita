#!/usr/bin/env bash
# Print DolRecomp-generated C (with PPC mnemonics in comments) for a guest PC
# range. Debug aid only; generated sources are gitignored.
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
exec python3 "$ROOT/scripts/show_guest_code.py" "$ROOT/generated/aot/generated/chunks" "$@"
