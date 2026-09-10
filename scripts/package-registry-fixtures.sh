#!/bin/sh
# Rebuilds fixture tarballs + metadata under public/packages/.
set -eu
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
exec python3 "$ROOT/scripts/package_registry_fixtures.py"
