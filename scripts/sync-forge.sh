#!/bin/sh
# One-way vendor sync: /c/Users/dooms/source/forge -> sunn/forge
# Preserves sunn/forge/SOURCE.md. Excludes build outputs and .git.
set -eu
UPSTREAM="${FORGE_UPSTREAM:-C:/Users/dooms/source/forge}"
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
DEST="$ROOT/forge"
BACKUP="$(mktemp)"
trap 'rm -f "$BACKUP"' EXIT
if [ -f "$DEST/SOURCE.md" ]; then cp "$DEST/SOURCE.md" "$BACKUP"; fi
if command -v rsync >/dev/null 2>&1; then
  rsync -a --delete \
    --exclude='.git/' --exclude='build/' --exclude='target/' \
    --exclude='*.exe' --exclude='*.o' --exclude='*.obj' \
    --exclude='*.a' --exclude='*.lib' \
    --exclude='.scratch/' --exclude='.opencode/' --exclude='examples/' \
    "$UPSTREAM/" "$DEST/"
else
  echo "rsync not found, falling back to cp (no delete). Install rsync for exact sync." >&2
  cp -R "$UPSTREAM/." "$DEST/"
fi
if [ -s "$BACKUP" ]; then cp "$BACKUP" "$DEST/SOURCE.md"; fi
echo "sync-forge: done. Update forge/SOURCE.md HEAD/date manually."
