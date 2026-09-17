#!/usr/bin/env bash
# Does the staged subset boot the same as the whole disc?
#
# The console never gets the 3.2 GB extraction. It gets the files named in
# generated/census.txt, and the census is produced by a host run that sees
# every file -- so a file the census loses is a file only the console will
# miss, and the symptom lands a long way from the cause: a failed disc read
# sends the guest down an error path and the boot wedges thousands of blocks
# later, on hardware, where there is no debugger. That is exactly how
# AudioRes/SMR.szs was lost (DiscEntry::first_seen came out of realloc
# uninitialised) and it cost days of hunting a "Vita-side stall" that was
# really a missing file.
#
# The check is the same one that found it: build a tree holding exactly what
# stage-media-vita.sh would send, boot it, boot the full extraction, and
# require the two to be indistinguishable. Symlinks, so no game bytes are
# copied and nothing leaves the user's machine.
#
#   ./scripts/check-staging.sh [blocks]
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
BLOCKS="${1:-2000000}"
GAME="${GV_GAME_ROOT:-$ROOT/generated/extracted/game}"
NAND="${GV_NAND_ROOT:-$ROOT/generated/nand}"
CENSUS="${GV_CENSUS:-$ROOT/generated/census.txt}"
BIN="$ROOT/generated/build/host-chassis/aot_chassis"
DOL="${GV_DOL:-$ROOT/generated/extracted/identity/sys/main.dol}"
OUT="$ROOT/generated/build/staging-check"

[[ -x "$BIN" ]] || { echo "no host chassis -- run ./scripts/run-host-chassis.sh first" >&2; exit 1; }
[[ -f "$CENSUS" ]] || { echo "no census at $CENSUS -- see scripts/stage-media-vita.sh" >&2; exit 1; }
[[ -d "$GAME/sys" ]] || { echo "no extracted disc at $GAME" >&2; exit 1; }

rm -rf "$OUT/mirror"
mkdir -p "$OUT/mirror/game" "$OUT/mirror/nand"

# The same system-area list stage-media-vita.sh sends unconditionally.
for f in sys/boot.bin sys/bi2.bin sys/apploader.img sys/main.dol sys/fst.bin \
         disc/header.bin disc/region.bin tmd.bin; do
  [[ -f "$GAME/$f" ]] || continue
  mkdir -p "$OUT/mirror/game/$(dirname "$f")"
  ln -sf "$GAME/$f" "$OUT/mirror/game/$f"
done
# The census records the paths as the run that wrote it saw them, which is
# relative to the repo root. A relative symlink target resolves against the
# link's own directory, not the cwd, so it has to be absolutised here or every
# link dangles and the mirror reads as a disc with no files on it at all.
while IFS= read -r f; do
  [[ -n "$f" ]] || continue
  abs="$f"
  [[ "$abs" = /* ]] || abs="$ROOT/$f"
  [[ -f "$abs" ]] || { echo "  census names a file that is not there: $f" >&2; continue; }
  rel="${abs#"$GAME"/}"
  [[ "$rel" != "$abs" ]] || rel="${abs#*/game/}"
  mkdir -p "$OUT/mirror/game/$(dirname "$rel")"
  ln -sf "$abs" "$OUT/mirror/game/$rel"
done < "$CENSUS"
if [[ -d "$NAND" ]]; then
  while IFS= read -r f; do
    rel="${f#"$NAND"/}"
    mkdir -p "$OUT/mirror/nand/$(dirname "$rel")"
    ln -sf "$f" "$OUT/mirror/nand/$rel"
  done < <(find "$NAND" -type f)
fi
echo "==> mirror: $(find "$OUT/mirror/game" -type l | wc -l) game files, $(find "$OUT/mirror/nand" -type l | wc -l) nand files"

echo "==> booting the staged subset ($BLOCKS blocks)"
GV_GAME_ROOT="$OUT/mirror/game" GV_NAND_ROOT="$OUT/mirror/nand" \
  "$BIN" "$DOL" "$BLOCKS" > "$OUT/subset.txt" 2>&1 || true
echo "==> booting the full extraction"
GV_GAME_ROOT="$GAME" GV_NAND_ROOT="$NAND" \
  "$BIN" "$DOL" "$BLOCKS" > "$OUT/full.txt" 2>&1 || true

rc=0
if grep -q '^  disc missing ' "$OUT/subset.txt"; then
  echo "check-staging: the boot asked for files the census does not list:" >&2
  grep '^  disc missing ' "$OUT/subset.txt" | sed "s#$OUT/mirror/game/##" >&2
  echo "  Regenerate it:  GV_DISC_CENSUS=generated/census.txt \\" >&2
  echo "                  ./scripts/run-host-chassis.sh \"$DOL\" 20000000" >&2
  rc=1
fi

# Counters are the sharper test: a subset that diverges at all diverges because
# a read came back short, whether or not the entry was nameable.
sub=$(grep '^stop=' "$OUT/subset.txt" || true)
full=$(grep '^stop=' "$OUT/full.txt" || true)
if [[ "$sub" != "$full" ]]; then
  echo "check-staging: the staged subset does not boot like the whole disc" >&2
  echo "  subset: $sub" >&2
  echo "  full:   $full" >&2
  rc=1
fi
[[ "$rc" -eq 0 ]] && echo "check-staging: ok ($BLOCKS blocks, identical to the full extraction)"
exit "$rc"
