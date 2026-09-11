# Forge

A build orchestration system for **C, C++, and Assembly**.

Forge exists to eliminate verbose hand-written build scripts and painful
cross-platform / cross-architecture compilation.
The goal is a build UX as simple as:

```sh
forge run --release
```

regardless of target OS, architecture, or toolchain.

## Philosophy

Forge takes heavy inspiration from **Cargo** — the same ergonomic,
"it just works" feel applied to a language ecosystem that has never really
had it.

Implementation language rules for Forge itself:

- **C** — primary, default language for everything.
- **C++** — only when a feature is materially harder or impossible in C.
- **Assembly** — only in hot paths worth hand-optimizing, not
  speculatively, and always commented so the optimization is legible
  rather than a black box.

(Rust was considered for implementing Forge itself, but dropped in favor
of raw C.)

## Project layout

- `src/cli.c` parses command-line arguments.
- `src/commands.c` owns the subcommand invocation lifecycles (`build`, `check`,
  `run`, `test`, `debug`, `clean`, `update`, `new`, `init`).
- `src/orchestrator.c` is the build engine: source discovery, compiler dispatch,
  output layout, and build execution (compiles run on a thread pool).
- `src/deps.c` resolves `[dependencies]`: git/path fetching, the shared cache,
  `Forge.lock` pinning, graph resolution, and foreign CMake/Make builds.
- `src/pkg.c` implements the manifest-editing dependency verbs
  (`forge add` / `forge remove`).
- `src/manifest.c`, `src/compiler.c`, `src/flags.c`, `src/log.c`, and
  `src/debug.c` provide the focused subsystems used by the build engine.
- `src/scaffold.c` generates new project skeletons for `forge new` / `forge init`.
- `src/flags.c` translates portable profile fields into each compiler's flag
  dialect (`-g`/`-O*` for GCC/Clang, `/Zi`/`/Od` for MSVC).
- `src/argv.c`, `src/process.c`, and `src/thread.c` provide the dynamic
  argument lists, shell-free process spawning, and thread pool primitives
  under the build engine.
- `src/forge_util.h` / `src/forge_util.c` hold small shared helpers (error
  formatting, trimming, PATH lookups) used across modules.
- `include/forge/` contains the interfaces between those modules.

## Building from source

Forge is written in C23 (built with `-std=c2x` for toolchain compatibility)
and builds itself with `gcc` or `clang` — no other
toolchain required. The only dependency is a C compiler.

### Make (Linux / macOS / Windows with GNU make)

```sh
make                    # builds build/forge
make test               # builds and runs `forge --help`
```

### Direct (no make)

```sh
# Linux / macOS
gcc -Wall -Wextra -Werror -std=c2x -Iinclude src/*.c -o build/forge

# Windows
gcc -Wall -Wextra -Werror -std=c2x -Iinclude src/*.c -o build\forge.exe
```

## Installation

Forge installs to a user-level bin directory — no `sudo` needed.

### Windows

Easiest (builds with the bundled `install.ps1`):

```powershell
powershell -ExecutionPolicy Bypass -File scripts\install.ps1
```

Or with GNU make (e.g. MSYS2 / Git Bash):

```sh
make CC=gcc          # mingw-w64 gcc; builds build/forge.exe
make install          # copies it to %USERPROFILE%\bin\forge.exe
```

Use the **mingw-w64** toolchain (package `mingw-w64-x86_64-gcc` on MSYS2), not
the MSYS2 runtime's own `gcc` ports compiler — Forge expects a native Windows
toolchain. `CC=gcc` selects it explicitly; otherwise GNU make falls back to
its built-in `cc`.

Installs to `%USERPROFILE%\bin\forge.exe` and prints the exact `PATH` command
to run (persist it with `setx` or the `SetEnvironmentVariable` snippet it
shows).

### Linux / macOS

```sh
sh scripts/install.sh     # or: chmod +x scripts/install.sh && ./scripts/install.sh
```

Installs to `~/.local/bin/forge` (override with `XDG_BIN_HOME`) and prints the
`export PATH=...` line to add to your shell profile.

### Via make

```sh
make install            # PREFIX defaults to ~/.local/bin (Linux/macOS)
make uninstall          # removes it again
```

After installing, add the bin directory to `PATH` and verify:

```sh
forge --help
```

## Quickstart

```sh
forge new my-app         # scaffold a project (Forge.toml + src/main.c)
cd my-app
forge run                # build (debug) and run
forge run --release      # build (release) and run
forge run -- --verbose   # arguments after -- go to your program
forge run -j 8           # compile up to 8 translation units in parallel
forge check              # fast compile-only validation, no link
forge test               # build & run every tests/*.c as its own binary
forge test --test alpha  # build & run only tests/alpha.c
forge debug              # build, then launch a debugger
forge clean              # remove the project's target/ output
forge add mylib --git https://github.com/example/mylib
                          # add a dependency, resolve it, and pin it
forge add local --path ../local
forge add hello --registry hello-c --version 0.1.0
                          # add a registry package (needs FORGE_REGISTRY_URL;
                          # without --version the newest release is pinned)
forge update mylib       # re-resolve one dep; `forge update` does all of them
forge remove mylib       # drop the entry and its lock pin
forge run --manifest path/to/Forge.toml
                          # build another project without cd-ing into it
```

Forge finds `Forge.toml` in the current directory or any parent, so you can
invoke it from a subdirectory of your project. `forge run` exits with your
program's own exit code.

Try it immediately against the bundled fixture:

```sh
cd test
forge run --release     # prints "Hello world!"
```

### Output style

Builds talk like Cargo. Milestones are right-aligned and green on a tty:

```text
   Compiling hello v0.1.0
    Finished release [optimized] target(s) in 0.42s
     Running target/release/hello.exe
Hello world!
```

A rerun with nothing to do prints only the `Finished` line. Verbosity:

- `-q` — errors and program output only.
- default — milestones; per-file detail stays in `target/logs/`.
- `-v` — also shows per-file compile/link progress (`up-to-date:`/`source:`).
- `-vv` — also echoes every compiler/linker command line.

`NO_COLOR=1` (or piping output) disables colors; everything always lands in
the invocation log under `target/logs/`.

### Reproducible resolution flags

`build`, `check`, `run`, `test`, and `debug` accept:

- `--offline` — never touch the network: cached clones with valid lock pins
  still resolve, anything else fails naming the dependency.
- `--locked` — fail instead of moving or adding a `Forge.lock` pin (CI guard:
  if this fails, someone forgot to commit an updated lockfile).
- `--frozen` — both at once.

`forge update` accepts `--offline` but rejects `--locked/--frozen`, since its
whole job is rewriting pins.

## Features

### Build orchestration
Simple, Cargo-style commands (`forge run`, `forge run --release`, ...)
replace verbose, hand-rolled Makefiles/build scripts.

`forge new <name>` scaffolds a ready-to-build project (manifest, hello-world
`src/main.c`, `.gitignore`) and validates the generated manifest with forge's
own parser; `forge init` does the same in the current directory, naming the
project after it.

Compilation is parallel: sources are compiled by a job pool sized to the host's
logical processors by default (`-j N` overrides; `-j auto` restores the
default). Compiles are independent, so a flat pool then a single link step
covers the dependency graph.

Incremental: already-compiled source files are skipped on rebuild. With GCC
and Clang, Forge asks the compiler to record the headers each translation
unit pulled in (`-MMD -MF`) and compares object mtimes against both the
source and every listed header, so touching a `.h` recompiles exactly the
files that use it. Toolchains that emit no dependency file (MSVC, assembly
sources) compare source mtimes only. On top of that, every object records a
hash of the exact compile command in an `<obj>.cmdhash` sidecar, so editing
`[profile.*] cflags` or bumping `[project] version` recompiles precisely the
objects whose commands changed — and objects from older forge versions
(without a stamp) recompile exactly once.

Processes are spawned directly with an argv array (`CreateProcess` /
`execvp`) — no shell is ever consulted, so paths and flags containing spaces
or metacharacters are passed literally. Commands that would exceed the OS
command-line limit spill the object list and flags into a compiler response
file (`@target/<profile>/link.rsp`) automatically.

Linking is incremental too: after a successful link, Forge records the exact
input list (objects, libraries, manifest) with each input's precise mtime in
a `<target>/<profile>/<exe>.linkstamp`. When nothing on that list changed,
the next build skips the link entirely (`up-to-date: <exe>`), so a rerun of
an already-built project is fully quiet — no compiles, no link.

### Fast validation and tests

`forge check` compiles every translation unit without linking — a quick
way to catch errors while editing, with incremental skips on repeat runs.

`forge test` builds each file in `tests/` as its own self-contained binary
(each file has its own `main`; C, C++, and assembly sources are all
collected), runs them all, prints a
`test result: N passed; M failed` summary, and exits nonzero when any fail.
A missing or empty `tests/` directory is not an error. Pass `--test NAME`
to build and run a single test (e.g. `forge test --test parser`) instead of
the whole suite; a filter that matches no test is an error.

### Dependencies

There is no npm-style registry for C — so forge treats **the VCS as the
registry**, like CPM, Meson wrapdb, and vcpkg all do in their own way:

```toml
[dependencies]
hello_lib = { path = "../libhello" }
coolib    = { git = "https://github.com/example/coolib", tag = "v1.2" }   # branch/rev also work
serde_c   = { registry = "serde-c", version = "0.1.0" }                   # omit version to track newest
```

- Git deps are cloned into a shared cache (`~/.forge/git`, override with
  `FORGE_HOME`) and pinned by resolved commit SHA in a generated `Forge.lock`
  — rebuilds after the first are quiet and offline-friendly. `forge update`
  re-resolves refs to the newest allowed state; naming a dep
  (`forge update coolib`) moves only that one past its pin.
- Because manifest URLs go straight onto the `git clone` command line, git
  URLs must use `https://`, `ssh://`, or scp-style `git@host:path` — anything
  else (local paths, `file://`, exotic transports) is rejected unless
  `FORGE_ALLOW_UNSAFE_GIT=1` is set in the environment.
- Dependencies resolve transitively (each dep may have its own
  `[dependencies]`); cycles are rejected with a clear error.
- A dependency is built with whatever it ships:
  - a `Forge.toml` → built natively by forge's own engine;
  - a `CMakeLists.txt` → `cmake` configure + build;
  - a `Makefile` → `make -jN CC=<dispatched compiler>`;
  - anything else is an error naming the dependency.
- Include directories (`<dep>/include`, else the dep root) feed every compile;
  the dep's objects/static library (`.a`/`.lib`) feed the link line.
  Dynamic libraries are out of scope for now.

**Trust model.** Resolving a dependency means trusting it: a foreign dep runs
its own `cmake`/`make` scripts on your machine, and forge makes that
explicit. The first time a foreign dep is built, forge asks before executing
its build scripts and records the decision next to the clone; a fresh clone
asks again.
`FORGE_ALLOW_DEP_BUILD_SCRIPTS=1` pre-approves everything (CI),
`=0` refuses outright. Git deps may also pull in their own git submodules
with `submodules = "true"`. Lockfile commit pins are validated as full SHAs,
so a hand-edited or corrupt `Forge.lock` fails loudly instead of checking
out something unexpected; dependencies fetched into the cache cannot use
path deps that escape their checkout; and two declarations of one dep name
pointing at different sources is an error rather than a silent race.

Dependencies can be managed without hand-editing the manifest, Cargo-style:

```sh
forge add mylib --git https://github.com/example/mylib --tag v1.2
forge add utils --path ../utils      # path deps are used in place, never cached
forge add hello --registry hello-c --version 0.1.0   # omit --version to pin newest
forge update mylib                   # pull just this dep to its newest allowed state
forge remove mylib                   # drop the entry and prune its lock pin
```

`add` inserts the `[dependencies]` entry while preserving every other byte of
the manifest (including comments and formatting), resolves the dependency
immediately so `Forge.lock` gains its pin, and rolls the edit back if
resolution fails (a bad URL or missing path never leaves a half-added dep).
Exactly one source (`--git` / `--path` / `--registry`) is accepted; git
sources may pin at most one ref (`--tag`, `--branch`, or `--rev`), registry
sources at most one `version` (`--version`). `remove` errors on unknown
names, listing the dependencies that do exist.

### Registry dependencies

Next to VCS sources, forge resolves source recipes from a sunn registry
(`FORGE_REGISTRY_URL`, e.g. `https://sunn.local`):

```toml
[dependencies]
hello = { registry = "hello-c", version = "0.1.0" }
```

- The registry maps `name@version` to either an upstream Git repository and
  ref, or a source archive URL and required sha256. Git recipes are checked
  out through the same shared Git cache as plain Git dependencies; URL
  recipes are verified before extraction. Registry pins use an explicit
  source kind in `Forge.lock`.
  A versioned entry never moves under `forge update` (only the manifest
  moves it); an unversioned `registry = "name"` entry tracks the newest
  release instead.
- Every URL download is checksum-verified before unpacking. Optional registry
  patches are fetched from `<registry>/patches/<name>` and applied before
  build-system detection. A recipe changed under a locked version fails
  loudly instead of following. `--offline` reuses warm checkouts only; `--locked` refuses
  any pin move — the same contract as git deps.
- Transports mirror the git allowlist: `https://` always, `http://` only
  for loopback, `file://` only with `FORGE_ALLOW_UNSAFE_REGISTRY=1`
  (local mirrors and the regression suite use it; a `file://` base
  points at the site root, same as `https://`).
- Downloads shell out to `curl` (PowerShell `Invoke-WebRequest` as a
  Windows fallback) and unpack with `tar` — no new libraries. On
  MSYS2/Git-Bash shells, make sure *native* curl/tar come first on PATH:
  the MSYS `/usr/bin/tar` cannot run as a grandchild of a native
  process (its gzip helper will not spawn).

Lock hygiene is automatic: entries for dependencies that disappear from the
manifest (directly removed, or dropped by a transitive consumer) are pruned
from `Forge.lock`, and clearing `[dependencies]` clears the lockfile.
`forge update` re-resolves every ref; `forge update NAME` re-resolves only
that dependency and leaves every other lock pin (and your offline rebuilds)
untouched.

### Cross-platform compiler dispatch
Forge detects the host OS and version at build time and automatically
invokes the platform's native compiler, falling back to `clang` when no
suitable native compiler is available.

### Manifest-based config
Project configuration lives in a custom manifest file, modeled on
`Cargo.toml` — declarative, not scripted.

Supported fields:

```toml
[project]
name = "app"                 # required
version = "0.1.0"            # optional: MAJOR.MINOR.PATCH (+ optional -prerelease);
                             # shown in status lines and injected as
                             # FORGE_PROJECT_VERSION ("0.1.0", a string macro)

[sources]
c = ["src"]                  # required: at least one of c/cpp/asm, non-empty
cpp = []                     # optional if unused
asm = []                     # optional if unused

[targets]
os = ["windows", "linux", "macos"]   # required
arch = ["x86_64", "aarch64"]         # required

[build]
compiler = "clang-17"        # optional: override compiler dispatch entirely

[profile.debug]
opt-level = 0                # 0..3, or omit for the compiler default
debug = true                 # debug info: -g / /Zi + /DEBUG
warnings-as-errors = true    # -Werror / /WX
std = "c11"                  # "c11", "c17", "c++20", ...; MSVC maps supported ones
cflags = ["-Wall"]           # optional raw extra flags (GCC/Clang dialect)

[profile.release]
opt-level = 2
std = "c17"
```

`targets.os`/`arch` list the hosts this project may build for; Forge builds
the current host only (no cross-compilation). Assembly: GCC/Clang toolchains
accept `.s`/`.S` sources; the MSVC-specific `.asm` dialect requires the MSVC
toolchain (`ml64`).

The `opt-level`, `debug`, `warnings-as-errors`, and `std` profile fields are
portable: each compiler backend translates them to its own dialect, so the
same manifest builds under GCC, Clang, and MSVC. Raw `cflags` are GCC/Clang
syntax; when MSVC is selected, common flags (`-g`, `-O*`, `-Wall`, `-Werror`,
`-std=`, `-D`/`-I`) are translated, and any flag with no MSVC equivalent is
rejected with a diagnostic pointing at the portable fields instead of being
silently passed through. A profile with no flags at all uses the compiler
defaults.

### Clearer debugging
The `debug` subcommand shells out to the target OS's native debugger
(or a chosen third-party one) and post-processes the output into
something more readable — e.g. a lifter-style disassembly/stack view
instead of a raw debugger dump.

Debugger selection, in order: `cdb` (Windows) → `gdb` (Windows/Linux) →
`lldb` (macOS). Override with the `FORGE_DEBUGGER` environment variable
(e.g. `FORGE_DEBUGGER=gdb`, `FORGE_DEBUGGER=windbg`).

### Verbose, readable logs
All build/run/debug output is written to a `/target` directory.
Logs are verbose by default and meant to be easy for a human to read,
not just machine-parsed. The terminal shows Cargo-style milestones by
default; every per-file detail (including full command lines) still goes
to `target/logs/` regardless of `-q/-v/-vv`.

## Scope

**v1 is build orchestration only.** Forge supports C, C++, and Assembly.

## Status

v1 build orchestration is implemented and working: `forge build`, `forge
check`, `forge run` (with `--` argument passthrough and exit-code
propagation), `forge test` (with a `--test NAME` filter), `forge debug`,
`forge clean`, `forge update`, `forge new`, and `forge init` cover the host
build loop. Path and git dependencies with lockfile pinning, Cargo-style
`forge add`/`forge remove` verbs, lock pruning, and foreign CMake/Make
dependencies are in place. Compiles and the final link are both incremental,
and flag/version changes invalidate exactly what they affect via per-object
command stamps. The console speaks Cargo (`Compiling`/`Finished`/`Running`
milestones, `-q/-v/-vv`, colors, timing) and dependency resolution honors
`--offline`/`--locked`/`--frozen`. Compiler dispatch, manifest parsing with
strict `[project] version`, upward manifest discovery, and per-invocation
`target/logs` are in place.

## Author note 

  I'm gonna keep it real. I don't have a very good 
understanding of C so i vibe-coded as much of a decent skeleton as i can while I'm learning C deeper.
This is very much a work in progress and I hope u guys are patient with me. :)

## License

TBD.
