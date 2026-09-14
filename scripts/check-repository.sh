#!/usr/bin/env bash
# Fail if tracked files look like Nintendo data or build products we must not ship.
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
cd "$ROOT"

if [[ ! -d .git ]]; then
  echo "not a git repo" >&2
  exit 1
fi

fail=0
while IFS= read -r f; do
  case "$f" in
    *.wbfs|*.iso|*.gcm|*.dol|*.rel|*.rso|*.vpk|eboot.bin)
      echo "FORBIDDEN tracked file: $f" >&2
      fail=1
      ;;
  esac
done < <(git ls-files)

# Large blobs over 20 MiB should not be in git.
while IFS= read -r line; do
  size=${line%%$'\t'*}
  path=${line#*$'\t'}
  if [[ "$size" -gt 20971520 ]]; then
    echo "FORBIDDEN large tracked file (${size} bytes): $path" >&2
    fail=1
  fi
done < <(git ls-files -z | xargs -0 -I{} stat -c $'%s\t%n' {})

if [[ "$fail" -ne 0 ]]; then
  echo "check-repository: FAIL" >&2
  exit 1
fi
echo "check-repository: ok"
