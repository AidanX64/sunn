# sunn

sunn is two registries in one place: a **shadcn component registry**
for the web UI, and a **native package registry for C, C++, and
assembly** in the vcpkg/conan spirit — installed with the **forge**
build tool (`forge run --release`, Cargo-style, no hand-written build
scripts).

Status: working skeleton. The web app runs, forge builds C/C++/ASM
projects with git/path/registry dependencies, and the native registry
serves fixture packages end to end. Binary releases and CI-built
artifacts are still ahead.

## Quickstart

Prerequisites: Node 20.18+ (22 recommended, see `.nvmrc`), pnpm 10+,
and a C compiler (`gcc`/`clang`; Windows: mingw-w64).

```sh
pnpm install
pnpm dev                    # web UI at http://localhost:3000
```

```sh
pnpm forge:build            # builds forge/build/forge(.exe)
pnpm forge:test             # forge --help smoke test
./forge/build/forge run --release --manifest forge/test/Forge.toml
```

```sh
# a registry dependency, end to end against the local fixtures:
export FORGE_REGISTRY_URL="file://$PWD/public"
export FORGE_ALLOW_UNSAFE_REGISTRY=1
```

## What's here

- `/` — landing page with a WebGPU shader hero plus the shadcn
  registry demos.
- `/packages` — browse/search native C/C++/ASM packages, with install
  tabs (`forge add`, `vcpkg`, `conan`, CMake FetchContent).
- `/forge` — what forge is and how to build the vendored copy.
- `/api/packages` — native package metadata
  (`?q=&lang=&triplet=`).
- `/api/forge/v1/resolve` — the contract the forge CLI resolves
  against (`?name=&version=&triplet=`).

## Layout

```
app/                  routes (/, /packages, /forge, /api/*, /registry/*)
components/           React (shader background, theme toggle, ui/*)
lib/                  shader engine + native-registry schema (zod)
registry/             shadcn distributables + registry.json
public/packages/      native registry hosting + fixture tarballs
public/r/             built shadcn output (from `shadcn build`)
registry-fixtures/    hello-c/cpp/asm library sources packed by script
forge/                vendored forge repo (see forge/SOURCE.md)
vendors/              future companions (openshaders, tweakcn, shadcn-ui)
scripts/              vendor sync + fixture packaging
```

## Native registry

Packages are versioned source tarballs with a sha256 per supported
triplet, described by JSON validated in `lib/sunn-registry.ts`. Today
it hosts fixtures (`hello-c`, `hello-cpp`, `hello-asm` — library-shaped
sources, never a `main`); per-triplet CI-built binaries are a later
phase. Rebuild everything from sources with:

```sh
pnpm packages:fixtures
```

Consume from any forge project once `FORGE_REGISTRY_URL` points here:

```toml
[dependencies]
hello = { registry = "hello-c", version = "0.1.0" }
```

```sh
forge add hello --registry hello-c --version 0.1.0
```

Pins land in `Forge.lock` as `{ version, sha256, url }`; every download
is checksum-verified before unpacking. `file://` registry URLs work for
local mirrors and tests with `FORGE_ALLOW_UNSAFE_REGISTRY=1`.

## forge (vendored)

`forge/` is a clean copy of the standalone forge repo; upstream stays
the source of truth, so edit there and re-sync — never the reverse:

```sh
pnpm sync:forge            # re-copy upstream (keeps forge/SOURCE.md)
make -C forge clean && make -C forge CC=gcc
```

Design, commands, and the test procedure are documented in
`forge/README.md` and `forge/AGENTS.md`.

## Troubleshooting

- `shadcn build` fails with `zod/v3` export errors: your install
  predates the `pnpm-workspace.yaml` overrides (pnpm ≥ 11 ignores
  `package.json#pnpm`). Run `pnpm install` fresh.
- `tar failed (exit 2)` from forge on Git-Bash/MSYS2: put native
  System32 `curl`/`tar` first on PATH — the MSYS `tar` cannot run as a
  grandchild of a native process.
- A forge build acting stale after header edits: `make -C forge clean`
  first, then rebuild.

## License

See [LICENSE](./LICENSE).
