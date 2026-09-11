# sunn native registry (static hosting)

This directory is served statically. Index: `sunn.registry.json`.
Each package version has `<version>.json` (validated by
`lib/sunn-registry.ts`) plus a `Forge.toml` manifest.

## Fixtures vs releases

`hello-c`, `hello-cpp`, `hello-asm` are **source fixtures** built by
`scripts/package_registry_fixtures.py` from `registry-fixtures/`.
Each fixture currently uses a URL recipe pointing at its source archive
and a real sha256. They exist so the Forge recipe client, lock pinning,
and install UX can be built and tested end to end.

Platform-specific binary artifacts are a later phase; they will be added
as a separate native artifact layer without changing source recipes.
Rebuild fixtures any time with:

```powershell
powershell -ExecutionPolicy Bypass -File scripts/package-registry-fixtures.ps1
```
