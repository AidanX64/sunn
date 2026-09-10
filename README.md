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

Prerequisites: Bun 1.4+ (pinned in `.bun-version`; Node 20.18+
still works as a fallback, see `.nvmrc`), and a C compiler
(`gcc`/`clang`; Windows: mingw-w64).

```sh
bun install
bun run dev                 # web UI at http://localhost:3000
```

```sh
bun run forge:build          # builds forge/build/forge(.exe)
bun run forge:test           # forge --help smoke test
./forge/build/forge run --release --manifest forge/test/Forge.toml
```

## Install from npm

Published as [`@v1dxu/sunn`](https://www.npmjs.com/package/@v1dxu/sunn)
(see [releases](https://github.com/AidanX64/sunn/releases)). Needs
Node 20.18+ or Bun 1.4+ on the host — no compiler, no build step:

```sh
npx @v1dxu/sunn serve --port 3100     # self-hosted registry at http://localhost:3100
npx @v1dxu/sunn init ./my-registry    # scaffold a working copy from the package
```

The npm package is self-contained: it includes the built server, both
registries, and the source tree needed to continue development. Versions
track git tags (`v0.1.0` → `0.1.0` on npm), and `init` copies those files
directly from the installed package without fetching GitHub.

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
bun run packages:fixtures
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
bun run sync:forge           # re-copy upstream (keeps forge/SOURCE.md)
make -C forge clean && make -C forge CC=gcc
```

Design, commands, and the test procedure are documented in
`forge/README.md` and `forge/AGENTS.md`.

## Troubleshooting

- `shadcn build` fails with `zod/v3` export errors: your install
  predates the `package.json#overrides` mixed-zod pinning. Run
  `bun install` fresh.
- `next dev` 500s on every route with a `globals.css` parse error
  (`--spacing(4)`): that is `tw-animate-css` (Tailwind v4 syntax) under
  the v3 pipeline — it fails identically on Node, so it is not a Bun
  issue. `dev` runs webpack (no `--turbopack` flag) until that import
  is resolved.
- `tar failed (exit 2)` from forge on Git-Bash/MSYS2: put native
  System32 `curl`/`tar` first on PATH — the MSYS `tar` cannot run as a
  grandchild of a native process.
- A forge build acting stale after header edits: `make -C forge clean`
  first, then rebuild.

## License

See [LICENSE](./LICENSE).
