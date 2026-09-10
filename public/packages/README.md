# sunn native registry (static hosting)

This directory is served statically. Index: `sunn.registry.json`.
Each package version has `<version>.json` (validated by
`lib/sunn-registry.ts`) plus a `Forge.toml` manifest.

## Fixtures vs releases

`hello-c`, `hello-cpp`, `hello-asm` are **source fixtures** built by
`scripts/package_registry_fixtures.py` from `registry-fixtures/`.
One source tarball per version is referenced by every supported
triplet with a real sha256. They exist so the forge registry client,
lock pinning, and install UX can be built and tested end to end.

Per-triplet **binary** artifacts built in CI are a later phase; when
they land, each triplet entry gets its own file+hash and this note
goes away. Rebuild fixtures any time with:

```powershell
powershell -ExecutionPolicy Bypass -File scripts/package-registry-fixtures.ps1
```
