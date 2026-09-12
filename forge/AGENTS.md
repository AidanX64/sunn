# AGENTS.md — Forge

## Project Overview

Forge is a build orchestration system for **C, C++, and assembly**. It exists
to reduce verbose build scripts and painful cross-platform /
cross-architecture compilation.

Design north star: Cargo. The build UX should be as close to

```
forge run --release
```

as possible, regardless of target OS, architecture, or toolchain.

## Language & Implementation Rules

- **C** is the primary implementation language. Default to it.
- **C++** only when a feature is materially harder or impossible in C.
  Justify the exception in the PR/commit description.
- **Assembly** only in hot paths identified as worth hand-optimizing —
  not speculatively. Comment *why* the asm exists, not just what it does.
- No Rust, despite Cargo being the UX inspiration — Rust was considered
  and dropped in favor of raw C.
- Prefer legible, well-commented code over clever code. Optimizations
  should be understandable, not black boxes.

## Core Subsystems

- **Build orchestration (v1 scope)**: build, link, run, clean, and debug.
- **Manifest**: custom manifest format, modeled on `Cargo.toml`. Keep
  parsing strict and errors human-readable.
- **Compiler dispatch**: detect host OS + version at build time and
  invoke the platform's native compiler, falling back to `clang` when
  no native compiler is available/appropriate.
- **Debugger integration**: the `debug` subcommand shells out to the
  target OS's native debugger (or a chosen third-party one) and
  post-processes its output into something clearer — e.g. a
  lifter-style disassembly/stack view instead of raw debugger dump.
- **Logging**: all build/run/debug output goes to a `/target`
  directory. Logs must be verbose by default and easy to read — no
  cryptic single-line failures.

## Conventions for Agents Working on This Repo

- Any new subcommand should compose with `forge run --release` style
  ergonomics — short, predictable, Cargo-like verbs.
- When touching compiler-dispatch code, handle the "unknown/unsupported
  OS" case explicitly rather than silently defaulting to clang.
- Keep `/target` log output structured enough to grep, but readable
  enough for a human skimming a failed build.

## Building and Testing

### Build
- `make CC=gcc` (or `make CC=clang`) builds `build/forge[.exe]`. The Makefile
  only defaults CC to gcc when the caller has not set it.
- New code must compile clean under `-Wall -Wextra -Werror -std=c2x`.
  `c2x` is the C23 draft name accepted by GCC >= 11 and Clang >= 15
  (ubuntu-latest still ships GCC 13, which rejects the final `c23` spelling;
  GCC >= 14 and Clang >= 16 accept both).
- On Windows use the mingw-w64 toolchain (MSYS2 MINGW64/ucrt64 gcc); the
  MSYS2 runtime's own `gcc` port produces the wrong target.

### Test — everything is verified in `test/`
`test/` is the canonical fixture (`test/Forge.toml`, a C project that prints
"Hello world!"). Run it after every change so your mental model matches CI:

1. `make clean && make CC=gcc` — clean full build of forge. (Always clean
   after touching `include/`: this Makefile now tracks headers with -MMD,
   but a clean build is still the ground truth.)
2. `build/forge.exe run --release --manifest test/Forge.toml` — expect
   `Hello world!` (Linux/macOS: `build/forge run --release`).
3. Repeat step 2 and expect *no* recompiles — the rerun log lists
   `up-to-date:` instead of `[compile] source:`. This is the incremental
   regression check.
4. `cd test && forge run` — exercises manifest discovery and
   project-root anchoring from a plain working directory.
5. `make regression` — the bash suites (`deps`, `registry`, engine). The
   registry suite builds a hermetic file:// stub, so dependency changes
   need it even when the fixture above is green.

### Subcommand conventions
- Subcommands: `build`, `check`, `run`, `test`, `debug`, `clean`, `update`,
  `new`, `init`. New ones must stay Cargo-like verbs and compose with
  `forge <verb> --release`.
- Build-style verbs share one options struct (`ForgeBuildOptions` in
  `include/forge/build.h`): release/max_jobs/offline/locked. Thread new
  build-affecting flags through it instead of growing positional params.
- `-q/-v/-vv` map onto `ForgeVerbosity` (log.c); milestone lines go through
  `forge_log_status`, per-file detail through `forge_logger_detail`,
  command echoes through `forge_logger_command`. The `target/logs/` file
  always receives everything — only the terminal is filtered.
- `--offline` forbids network in dependency resolution; `--locked` refuses to
  change any Forge.lock pin; `--frozen` is both. `forge update` accepts
  `--offline` but must reject `--locked/--frozen` (its job is rewriting pins).
- `[project] version = "MAJOR.MINOR.PATCH"` is validated strictly and
  injected as a quoted-string macro `FORGE_PROJECT_VERSION`; changing it — or
  any `[profile.*] cflags` entry — invalidates objects via the
  `<obj>.cmdhash` sidecars, so never "optimize away" the command-hash check.
- `forge run -- ARGS...` passes ARGS to the program and propagates its exit
  code; keep that contract for anything that spawns user programs.
- `forge test` builds each `tests/*.c` as a self-contained binary (own
  `main`); missing/empty `tests/` exits 0.
- `[dependencies]` supports path deps, git deps pinned by commit in a
  generated `Forge.lock`, and registry deps (`registry = "name"` with
  `version` exact, `min-version`, `version-range` (`= >= > <= < ^ ~`,
  wildcards, `,` AND, `||` OR), and/or `max-version`) pinned by version
  + tarball sha256; top-level `[overrides]` force one exact version per
  package (transitive manifests must not declare them). Per-dep
  `cmake-args`/`cmake-toolchain`/`make-args`/`make-target` tune foreign
  builds (argv only, metachars rejected; args change forces a clean
  foreign rebuild tracked by `.forge-foreign-args`); `FORGE_OVERLAYS`
  names local site roots shadowing the registry. Foreign deps are
  detected as CMake or Make and must produce a static library.
- The forge version string lives in `include/forge/version.h`.

### Incremental-build caveats
- Objects are skipped when the source and every header the compiler recorded
  (`-MMD -MF` depfiles, `target/{debug,release}/obj/*.d`) are older than the
  object AND the recorded compile-command hash matches the current one
  (`<obj>.cmdhash`). Editing a `.h` recompiles only the translation units
  that include it; editing profile cflags or the project version recompiles
  exactly the objects whose command changed.
- Objects built by an older forge have no depfile/cmdhash stamp and recompile
  exactly once.
- If an incremental run ever looks wrong, force a clean build first
  (`forge clean`, or `rm -rf build target test/target`).
- MSVC and assembly sources have no header tracking (source mtime + command
  hash only); MSVC is untestable on this machine, so the gcc fallback is what
  tests and CI cover.

### Diagnostics
- Everything lands in `target/logs/`, and a failed stage also prints its last
  log lines to stderr. Read those before rerunning or "fixing" a build.

## Scope (v1)

- C/C++/assembly language support
