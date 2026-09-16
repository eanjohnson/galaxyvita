#!/usr/bin/env bash
# Apply the local patches to third_party/ModernGekko.
#
# The submodule is pinned by SHA, so the parent repo cannot record edits to its
# working tree. The patches live here instead and this script reapplies them.
# It is idempotent: a patch that is already applied is skipped, so it is safe to
# run on every build.
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
SUB="$ROOT/third_party/ModernGekko"

if [[ ! -e "$SUB/.git" ]]; then  # a submodule's .git is a file, not a directory
  echo "third_party/ModernGekko is not checked out -- run:" >&2
  echo "  git submodule update --init --recursive" >&2
  exit 1
fi

rc=0
for patch in "$ROOT"/patches/moderngekko-*.patch; do
  [[ -e "$patch" ]] || continue
  name="$(basename "$patch")"
  if git -C "$SUB" apply --check --reverse "$patch" 2>/dev/null; then
    echo "already applied: $name"
  elif git -C "$SUB" apply "$patch" 2>/dev/null; then
    echo "applied:         $name"
  else
    echo "FAILED:          $name (does not apply to $(git -C "$SUB" rev-parse --short HEAD))" >&2
    rc=1
  fi
done
exit $rc
