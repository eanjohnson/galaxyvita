#!/usr/bin/env bash
# Puts the user's own extracted disc on their own console, over vitacompanion.
#
# The extraction is 3.2 GB and the boot touches 25 MB of it. Staging only what
# the boot asks for turns a 40-minute transfer into a 30-second one, and the
# list is not guessed -- tests/host/aot_chassis.c records which files the game
# pulled, in load order, when GV_DISC_CENSUS names a file:
#
#   GV_DISC_CENSUS=census.txt ./scripts/run-host-chassis.sh <dol> 60000000
#   ./scripts/stage-media-vita.sh census.txt
#
# Without a census every file under files/ is sent, which is correct but slow.
# No game bytes pass through this repository; source and destination are both
# the user's.
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
CENSUS="${1:-}"
GAME="${GV_GAME_ROOT:-$ROOT/generated/extracted/game}"
NAND="${GV_NAND_ROOT:-$ROOT/generated/nand}"
# shellcheck disable=SC1091
source "$ROOT/scripts/vita-env.sh"

exec 9>"${TMPDIR:-/tmp}/galaxyvita-device.lock"
flock -n 9 || { echo "another script is using the Vita; wait for it or kill it" >&2; exit 1; }

[[ -n "${GALAXYVITA_IP:-}" ]] || { echo "Set GALAXYVITA_IP or write scripts/.vita-ip" >&2; exit 1; }
IP="$GALAXYVITA_IP"
DEST="ux0:/data/galaxyvita"

[[ -d "$GAME/sys" ]] || { echo "no extracted disc at $GAME -- run ./scripts/extract-disc.sh" >&2; exit 1; }

put() {
  curl -sS --connect-timeout 20 --ftp-create-dirs -T "$1" "ftp://$IP:1337/$2"
}

# Sent one at a time rather than as one recursive push: vitacompanion's FTP has
# no mkdir -p of its own, and --ftp-create-dirs per file is what builds the
# tree. Progress is printed because a silent five-minute transfer looks hung.
n=0
stage() { # <local> <disc-relative>
  put "$1" "$DEST/game/$2"
  n=$((n + 1))
  if (( n % 25 == 0 )); then echo "    $n files"; fi
}

echo "==> disc system area"
for f in sys/boot.bin sys/bi2.bin sys/apploader.img sys/main.dol sys/fst.bin \
         disc/header.bin disc/region.bin tmd.bin; do
  [[ -f "$GAME/$f" ]] || { echo "missing $GAME/$f" >&2; exit 1; }
  stage "$GAME/$f" "$f"
done

echo "==> NAND tree"
if [[ -d "$NAND" ]]; then
  while IFS= read -r f; do
    rel="${f#"$NAND"/}"
    put "$f" "$DEST/nand/$rel"
  done < <(find "$NAND" -type f)
fi

if [[ -n "$CENSUS" ]]; then
  [[ -f "$CENSUS" ]] || { echo "no census at $CENSUS" >&2; exit 1; }
  total=$(wc -l < "$CENSUS")
  echo "==> $total files from the census"
  while IFS= read -r f; do
    [[ -n "$f" ]] || continue
    # The census records host paths; the disc-relative part is what the console
    # needs, and it is whatever follows the game root.
    rel="${f#"$GAME"/}"
    [[ "$rel" != "$f" ]] || rel="${f#*/game/}"
    [[ -f "$f" ]] || { echo "  skipping missing $rel" >&2; continue; }
    stage "$f" "$rel"
  done < "$CENSUS"
else
  echo "==> every file under files/ (no census given; this is the slow path)"
  while IFS= read -r f; do
    stage "$f" "${f#"$GAME"/}"
  done < <(find "$GAME/files" -type f)
fi

echo "==> staged $n files"
