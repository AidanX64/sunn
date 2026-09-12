# forge — vendored snapshot (sunn monorepo)

This is a clean vendor copy of the standalone `forge` repo. The upstream
repo remains the source of truth — do not edit C sources here directly,
edit upstream and re-sync.

- Upstream repo: `https://github.com/AidanX64/forge.git` (branch `master`)
- Local checkout: `C:\Users\dooms\source\forge`
- Upstream HEAD: `32ce436416275520176da2abb1392749dc94c899`
- Upstream status at copy time: dirty (uncommitted feature work, see below)
- Copied: 2026-09-11 (robocopy, working tree)
- Excluded from copy: `.git/`, `.github/`, `build/`, `target/`, `test/target/`, `*.exe`, `*.o`, `*.obj`, `*.a`, `*.lib`, `.scratch/`, `.opencode/`, `examples/`
- CI ownership: upstream keeps its own `.github/`; `sunn/.github/workflows/ci.yml`
- is the only CI in this repo and tests the vendored tree as integrated.

Upstream `git status --short` at copy time:

```text
 M AGENTS.md
 M README.md
 M include/forge/deps.h
 M include/forge/manifest.h
 M include/forge/orchestrator.h
 M include/forge/pkg.h
 M include/forge/registry.h
 M src/cli.c
 M src/commands.c
 M src/deps.c
 M src/manifest.c
 M src/pkg.c
 M src/registry.c
 M test/deps-regression.sh
 M test/registry-regression.sh
```

Note: this snapshot includes the committed sunn-registry client
(`registry = "pkg"` deps, tarball download + sha256 verify, registry
Forge.lock pins, `forge add --registry/--version`,
`test/registry-regression.sh`, Makefile header tracking). The untracked
`test/regression.sh` also carries an updated R1 assertion for the new
source-count message.

This snapshot additionally carries uncommitted upstream work (to be
committed upstream as its own change): full version-range requirements
(`version-range` with `= >= > <= < ^ ~` wildcards, `,` AND, `||` OR,
plus `max-version` and top-level `[overrides]`), `FORGE_OVERLAYS`
local-port shadowing, per-dependency foreign-build args
(`cmake-args`/`cmake-toolchain`/`make-args`/`make-target` with
args-change rebuild tracking), a `git+` registry-base hint, and new
regression coverage R16–R21 plus K8 (upstream `make regression` green).

## Sync

One-way refresh from upstream (overwrites this directory, preserves this file):

```powershell
powershell -ExecutionPolicy Bypass -File scripts/sync-forge.ps1
```

```sh
sh scripts/sync-forge.sh
```

After syncing, update the HEAD/status block above.

## Build (per forge/AGENTS.md)

```sh
make -C forge CC=gcc
./forge/build/forge run --release --manifest forge/test/Forge.toml
```

On Windows use the mingw-w64 toolchain (MSYS2 MINGW64/ucrt64 gcc),
not the MSYS2 runtime gcc. Flags must stay `-Wall -Wextra -Werror -std=c2x`.
On MSYS2/Git-Bash shells, native System32 curl/tar must precede MSYS ones
on PATH (MSYS tar cannot run as a grandchild of a native process).
