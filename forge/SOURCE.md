# forge — vendored snapshot (sunn monorepo)

This is a clean vendor copy of the standalone `forge` repo. The upstream
repo remains the source of truth — do not edit C sources here directly,
edit upstream and re-sync.

- Upstream path: `C:\Users\dooms\source\forge`
- Upstream HEAD: `dc66b09c3bc09d481793fc4276ec69c5ca09f3f2`
- Upstream status at copy time: dirty (see below)
- Copied: 2026-09-09 (robocopy, working tree)
- Excluded from copy: `.git/`, `build/`, `target/`, `test/target/`, `*.exe`, `*.o`, `*.obj`, `*.a`, `*.lib`, `.scratch/`, `.opencode/`, `examples/`

Upstream `git status --short` at copy time:

```text
M .github/workflows/ci.yml
M AGENTS.md
M Makefile
M README.md
M include/forge/deps.h
M include/forge/manifest.h
M include/forge/orchestrator.h
M include/forge/pkg.h
M src/argv.c
M src/cli.c
M src/commands.c
M src/compiler.c
M src/deps.c
M src/fingerprint.c
M src/flags.c
M src/forge_util.c
M src/forge_util.h
M src/log.c
M src/manifest.c
M src/orchestrator.c
M src/paths.c
M src/pkg.c
M src/scaffold.c
M src/sources.c
M test/deps-regression.sh
?? include/forge/fetch.h
?? include/forge/registry.h
?? include/forge/sha256.h
?? src/fetch.c
?? src/registry.c
?? src/sha256.c
?? test/registry-regression.sh
?? test/regression.sh
```

Note: this snapshot includes the uncommitted sunn-registry client
(`registry = "pkg"` deps, tarball download + sha256 verify, registry
Forge.lock pins, `forge add --registry/--version`,
`test/registry-regression.sh`, Makefile header tracking). The untracked
`test/regression.sh` also carries an updated R1 assertion for the new
source-count message.

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
