# forge — canonical source (sunn monorepo)

This directory is the canonical Forge source tree. The standalone
`forge` checkout/repository is a downstream mirror — do not make
independent source changes there; mirror the exact Sunn commit instead.

- Canonical repo: `https://github.com/AidanX64/sunn.git` (`forge/` subtree, branch `main`)
- Mirror repo: `https://github.com/AidanX64/forge.git` (branch `master`)
- Canonical commit: `fd02b7c1ba9c9621e00ba9ae695599d4c723a402`
- Canonical status at mirror time: clean
- Mirrored: 2026-09-12 (exact Sunn commit, `forge/` subtree)
- Excluded from mirror: `forge/SOURCE.md` (Sunn provenance), `.git/`, `.github/`, `build/`, `target/`, `test/target/`, `*.exe`, `*.o`, `*.obj`, `*.a`, `*.lib`, `.scratch/`, `.opencode/`, `examples/`
- Preserved in mirror: `.git/`, `.github/`, ignored build outputs, and standalone-only `MIRROR.md`
- CI ownership: Sunn tests the canonical tree via `sunn/.github/workflows/ci.yml`; the standalone mirror keeps its own CI for mirror verification.

Sunn `git status --short -- forge` at mirror time:

```text
(clean)
```

This tree includes the sunn-registry client
(`registry = "pkg"` deps, tarball download + sha256 verify, registry
Forge.lock pins, `forge add --registry/--version`,
`test/registry-regression.sh`, Makefile header tracking), full
version-range requirements (`version-range` with `= >= > <= < ^ ~`
wildcards, `,` AND, `||` OR, plus `max-version` and top-level
`[overrides]`), `FORGE_OVERLAYS` local-port shadowing, per-dependency
foreign-build args (`cmake-args`/`cmake-toolchain`/`make-args`/`make-target`
with args-change rebuild tracking), a `git+` registry-base hint, and new
regression coverage R16–R21 plus K8 (`make regression` green).

## Mirror

One-way mirror from this canonical directory into the standalone checkout:

```powershell
powershell -ExecutionPolicy Bypass -File scripts/mirror-forge.ps1 --Apply
```

```sh
sh scripts/mirror-forge.sh --apply
```

Preview or verify without changing the mirror:

```powershell
powershell -ExecutionPolicy Bypass -File scripts/mirror-forge.ps1 --Check
```

```sh
sh scripts/mirror-forge.sh --check
```

The old upstream-first workflow is retired. Do not run
`scripts/sync-forge.ps1` or `scripts/sync-forge.sh` to overwrite this
canonical tree.

## Build (per forge/AGENTS.md)

```sh
make -C forge CC=gcc
./forge/build/forge run --release --manifest forge/test/Forge.toml
```

On Windows use the mingw-w64 toolchain (MSYS2 MINGW64/ucrt64 gcc),
not the MSYS2 runtime gcc. Flags must stay `-Wall -Wextra -Werror -std=c2x`.
On MSYS2/Git-Bash shells, native System32 curl/tar must precede MSYS ones
on PATH (MSYS tar cannot run as a grandchild of a native process).
