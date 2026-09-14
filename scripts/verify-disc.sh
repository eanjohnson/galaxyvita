#!/usr/bin/env bash
# Hash a user-supplied image. Writes docs/DISC-IDENTITY.md. Never copies game bytes into git.
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/.." && pwd)"

IMAGE="${1:-}"
if [[ -z "$IMAGE" ]]; then
  # Default to the workspace backup if present.
  shopt -s nullglob
  candidates=("$ROOT"/*.wbfs "$ROOT"/ref/rom/*.{wbfs,iso,ISO})
  shopt -u nullglob
  if [[ ${#candidates[@]} -eq 1 ]]; then
    IMAGE="${candidates[0]}"
  else
    echo "usage: $0 <path-to-rmge01.wbfs>" >&2
    exit 1
  fi
fi
if [[ ! -f "$IMAGE" ]]; then
  echo "not a file: $IMAGE" >&2
  exit 1
fi

SIZE=$(stat -c '%s' "$IMAGE")
SHA256=$(sha256sum -- "$IMAGE" | awk '{print $1}')
SHA1=$(sha1sum -- "$IMAGE" | awk '{print $1}')
NAME=$(basename -- "$IMAGE")

OUT="$ROOT/docs/DISC-IDENTITY.md"
cat > "$OUT" <<EOF
# Disc identity

Recorded $(date -u +%Y-%m-%dT%H:%M:%SZ). The original file was not modified.

| Field | Value |
|---|---|
| Filename | \`$NAME\` |
| Bytes | $SIZE |
| SHA-256 | \`$SHA256\` |
| SHA-1 | \`$SHA1\` |

GalaxyPad RMGE01 rev 0 container SHA-256 is
\`bd0d3d4bc1376a8614fd8f4fee5df86be6bc676e9d617944918fda79ed9e3589\`
(3,508,535,296 bytes). Compare before treating this dump as the same pin.

Extraction, title ID, revision, and \`sys/main.dol\` hashes are filled after
\`scripts/extract-disc.sh\` exists and \`wit\`/DolRecomp run.

Do not paste game bytes here.
EOF

python3 - "$ROOT/config/galaxyvita-disc.json" "$SIZE" "$SHA256" <<'PY'
import json, sys
path, size, sha = sys.argv[1], int(sys.argv[2]), sys.argv[3]
with open(path, encoding="utf-8") as f:
    data = json.load(f)
data["local"]["sizeBytes"] = size
data["local"]["sha256"] = sha
ref = data.get("galaxyPadReference", {}).get("containerSha256")
data["supportState"] = "container-hashed"
if ref and sha == ref:
    data["supportState"] = "container-matches-galaxypad-rmge01-rev0"
with open(path, "w", encoding="utf-8") as f:
    json.dump(data, f, indent=2)
    f.write("\n")
PY

echo "Wrote $OUT"
echo "size=$SIZE sha256=$SHA256"
