#!/bin/sh
# One-way mirror: sunn/forge (canonical) -> standalone forge checkout.
# Default is --check (report drift, change nothing).
#   sh scripts/mirror-forge.sh --check
#   sh scripts/mirror-forge.sh --apply
#   sh scripts/mirror-forge.sh --apply --commit --push
# Options: --mirror DIR --ref REV --check --apply --commit --push
set -eu
MIRROR="${FORGE_MIRROR:-C:/Users/dooms/source/forge}"
REF="HEAD"
MODE="check"
DO_COMMIT=0
DO_PUSH=0
while [ "$#" -gt 0 ]; do
  case "$1" in
    --mirror) MIRROR="$2"; shift 2 ;;
    --ref) REF="$2"; shift 2 ;;
    --check) MODE="check"; shift ;;
    --apply) MODE="apply"; shift ;;
    --commit) DO_COMMIT=1; shift ;;
    --push) DO_PUSH=1; shift ;;
    *) echo "unknown option: $1" >&2; exit 2 ;;
  esac
done
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
SUB="forge"
sha="$(git -C "$ROOT" rev-parse --verify "$REF")"
commit_date="$(git -C "$ROOT" show -s --format=%cI "$sha")"
sunn_branch="$(git -C "$ROOT" rev-parse --abbrev-ref HEAD)"
if [ -n "$(git -C "$ROOT" status --porcelain -- "$SUB")" ]; then
  echo "Refusing mirror: sunn/$SUB has uncommitted changes. Commit them first." >&2
  exit 1
fi
[ -d "$MIRROR" ] || { echo "Mirror checkout not found: $MIRROR" >&2; exit 1; }
git -C "$MIRROR" rev-parse --git-dir >/dev/null
[ -z "$(git -C "$MIRROR" status --porcelain)" ] || { echo "Refusing mirror: destination checkout is dirty." >&2; exit 1; }
mirror_branch="$(git -C "$MIRROR" rev-parse --abbrev-ref HEAD)"
STAGE="$(mktemp -d)"
trap 'rm -rf "$STAGE"' EXIT
git -C "$ROOT" archive --format=tar --output="$STAGE/forge.tar" "$sha" "$SUB"
tar -xf "$STAGE/forge.tar" -C "$STAGE"
MANIFEST="$STAGE/manifest.txt"
git -C "$ROOT" ls-tree -r --name-only "$sha" -- "$SUB" | sed 's|^forge/||' | grep -v '^SOURCE.md$' | grep -v '^$' | sort >"$MANIFEST"
if command -v sha256sum >/dev/null 2>&1; then
  shasum() { sha256sum "$1" | cut -d' ' -f1; }
else
  shasum() { shasum -a 256 "$1" | cut -d' ' -f1; }
fi
# Line-ending-insensitive hash: checkouts may carry CRLF while git blobs
# carry LF (core.autocrlf / text=auto). Strip CR bytes before hashing so
# identical content compares equal on every platform.
normsha() { tr -d '\r' <"$1" | sha256sum 2>/dev/null | cut -d' ' -f1; }
if ! command -v sha256sum >/dev/null 2>&1; then
  normsha() { tr -d '\r' <"$1" | shasum -a 256 | cut -d' ' -f1; }
fi
missing=0; changed=0; removed=0
missing_list=""; changed_list=""; removed_list=""
DESTLIST="$STAGE/dest.txt"
git -C "$MIRROR" ls-files | sort >"$DESTLIST"
while IFS= read -r rel; do
  [ -n "$rel" ] || continue
  # Case-sensitive membership first: the filesystem may be
  # case-insensitive, hiding case-only renames (Forge.toml vs forge.toml).
  if ! grep -qxF "$rel" "$DESTLIST"; then
    missing=$((missing+1)); missing_list="$missing_list $rel"; continue
  fi
  if [ ! -f "$MIRROR/$rel" ]; then
    missing=$((missing+1)); missing_list="$missing_list $rel"; continue
  fi
  a="$(normsha "$STAGE/$SUB/$rel")"; b="$(normsha "$MIRROR/$rel")"
  if [ "$a" != "$b" ]; then changed=$((changed+1)); changed_list="$changed_list $rel"; fi
done <"$MANIFEST"
while IFS= read -r rel; do
  [ -n "$rel" ] || continue
  case "$rel" in
    MIRROR.md|.github/*|build/*|target/*|test/target/*|examples/*|.scratch/*|.opencode/*|*.exe|*.o|*.obj|*.a|*.lib) continue ;;
  esac
  if ! grep -qxF "$rel" "$MANIFEST"; then
    removed=$((removed+1)); removed_list="$removed_list $rel"
  fi
done <<EOF
$(git -C "$MIRROR" ls-files)
EOF
HASHLIST="$STAGE/hashlist.txt"
while IFS= read -r rel; do [ -n "$rel" ] || continue; printf '%s  %s\n' "$(normsha "$STAGE/$SUB/$rel")" "$rel"; done <"$MANIFEST" >"$HASHLIST"
root_hash="$(shasum "$HASHLIST")"
mirror_note="$(cat <<EOF
# forge - downstream mirror

This checkout is a read-only mirror. sunn/forge is canonical - do not edit sources here.

- Canonical repo: https://github.com/AidanX64/sunn.git (forge/ subtree, branch $sunn_branch)
- Canonical commit: $sha ($commit_date)
- Canonical files: $(wc -l <"$MANIFEST" | tr -d ' ')
- Canonical tree hash: $root_hash
- Mirrored: $(date -u +%F) (mirror-forge)
EOF
)"
note_differs=0
# The working tree may carry CRLF (text=auto checkouts); normalize before
# comparing so identical notes compare equal on every platform.
if [ ! -f "$MIRROR/MIRROR.md" ] || [ "$(tr -d '\r' <"$MIRROR/MIRROR.md")" != "$mirror_note" ]; then note_differs=1; fi
echo "canonical: sunn@$sha"
echo "mirror:    $MIRROR (branch $mirror_branch)"
echo "missing-in-mirror: $missing; changed: $changed; removed-upstream: $removed; mirror-note-differs: $note_differs"
if [ "$MODE" = "check" ]; then
  # A stale mirror note alone is informational; only content drift fails.
  if [ "$missing" -eq 0 ] && [ "$changed" -eq 0 ] && [ "$removed" -eq 0 ]; then
    echo "mirror-forge: clean."
    exit 0
  fi
  exit 1
fi
for rel in $removed; do rm -f "$MIRROR/$rel"; done
while IFS= read -r rel; do
  [ -n "$rel" ] || continue
  mkdir -p "$MIRROR/$(dirname "$rel")"
  cp -f "$STAGE/$SUB/$rel" "$MIRROR/$rel"
done <<EOF
$(printf '%s\n' $missing_list $changed_list | tr ' ' '\n' | grep -v '^$' | sort -u)
EOF
printf '%s\n' "$mirror_note" >"$MIRROR/MIRROR.md"
if [ "$DO_COMMIT" -eq 1 ] || [ "$DO_PUSH" -eq 1 ]; then
  git -C "$MIRROR" add -A
  if [ -n "$(git -C "$MIRROR" status --porcelain)" ]; then
    git -C "$MIRROR" commit -m "Mirror sunn/forge from sunn@$sha" -m "Canonical: sunn $sunn_branch@$sha ($commit_date)."
  else
    echo "mirror-forge: applied; nothing new to commit."
  fi
  [ "$DO_PUSH" -eq 0 ] || git -C "$MIRROR" push origin "$mirror_branch"
fi
echo "mirror-forge: applied."
