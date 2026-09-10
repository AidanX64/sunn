# AGENTS.md — sunn

## Project overview

sunn is a monorepo with one web app at the repo root and vendored
companions. Long-term vision: half shadcn registry, half vcpkg/conan-style
registry for C/C++/ASM, powered by the forge build tool. All of it is
called sunn.

- **Web** (this repo root): Next.js 15 + Tailwind v3 + shadcn. A shadcn
  component registry (`registry/` + `registry.json`) next to a native
  C/C++/ASM package registry (`public/packages/` + `/packages` UI +
  `/api/*`). The landing hero runs a WebGPU shader.
- **`forge/`**: vendored copy of the standalone forge repo (Cargo-like
  build orchestration for C/C++/assembly, written in C). Upstream at
  `C:\Users\dooms\source\forge` is the source of truth —
  **never edit `forge/` C sources directly**; change upstream, verify
  there, then `sync-forge` (see below).
- **`vendors/`**: reserved slots (`openshaders`, `tweakcn`, `shadcn-ui`).
  Decision records live in each README; nothing is vendored there yet.

`forge/AGENTS.md` governs all C work (language rules, subcommand
conventions, test procedure). This file governs the monorepo around it.

## Layout

```
app/                  Next.js routes (/, /packages, /forge, /api/*, /registry/*)
components/           local React (sunn-shader-background, mode-toggle, ui/*)
lib/                  custom-shaders.js (WebGPU engine) + sunn-registry.ts (zod)
registry/             shadcn distributables + registry.json
public/packages/      native registry hosting (sunn.registry.json + per-version JSON)
public/r/             built shadcn registry output (checked in, from `shadcn build`)
registry-fixtures/    fixture sources (hello-c/cpp/asm) packed by script
forge/                vendored forge (see forge/SOURCE.md)
vendors/              future companions (READMEs only for now)
scripts/              sync-forge.*, package-registry-fixtures.*
```

## Commands (run from repo root)

```sh
bun run dev                 # web dev server (webpack; see Turbopack gotcha)
bun run build               # Next.js production build (runs typecheck)
bunx tsc --noEmit           # fast typecheck
bun run registry:build      # shadcn build -> public/r
bun run packages:fixtures   # rebuild fixture tarballs + public/packages metadata
bun run forge:build         # make -C forge CC=gcc
bun run forge:test          # make -C forge test
bun run forge:clean         # make -C forge clean
bun run sync:forge          # re-copy upstream forge (preserves forge/SOURCE.md)
```

Full C verification lives upstream (`make clean && make CC=gcc`,
fixture runs, `make regression`); see `forge/AGENTS.md`.

## Conventions for agents working in this repo

- **Web stays at root.** Do not move it to `apps/web`: `@/*` aliases,
  `components.json`, and `shadcn build` paths all assume root.
- **No Turborepo.** `bun run` scripts + `make -C forge` is the orchestration.
  Revisit only when a second JS buildable appears.
- **Never run pnpm/npm in this repo.** pnpm v12 auto-recreates
  `pnpm-lock.yaml` + `pnpm-workspace.yaml` on any invocation
  (`verify-deps-before-run`), dirtying the tree behind your back. Use
  `bun run …` for JS and `make -C forge …` directly for C (the
  `forge:*` scripts are plain make passthroughs).
- **Two registries, don't mix them.** `registry.json` is shadcn-owned
  (`shadcn build` breaks on unknown item types). Native packages live in
  `public/packages/sunn.registry.json`, validated by
  `lib/sunn-registry.ts`. The forge resolve contract is
  `GET /api/forge/v1/resolve?name=&version=&triplet=`.
- **Install settings live in `package.json`, NOT `pnpm-workspace.yaml`**
  (Bun ignores that file, and it is deleted). Overrides plus
  `trustedDependencies` (`sharp`, `unrs-resolver`) live in
  `package.json`. The zod tree is intentionally mixed: v3 at root
  (shadcn's schemas call `.deepPartial()`, removed in v4) and v4 under
  `@modelcontextprotocol/sdk` (1.30 imports `zod/v3`, which only exists
  in the v4 package). Do not "unify" it without running
  `bun run registry:build`.
- **Bun-first:** Bun 1.4+ pinned in `.bun-version` (Next CLIs run via
  `bun --bun`). Node `>= 20.18.1` remains the fallback floor
  (`.nvmrc`); the tree was last verified on Node 24.
  `bunx shadcn build` is the canary.
- **Shader seam:** `lib/custom-shaders.js` is the engine,
  `components/sunn-shader-background.tsx` the wrapper (theme sync,
  WebGPU-missing fallback, cleanup). Openshaders integration, when it
  happens, goes behind the wrapper's props — not a rewrite of call sites.
- **Fixtures, not releases:** `hello-c/cpp/asm` are library-shaped
  sources (objects, never a `main` — deps link every object). Tarballs
  are per-version sources, one file referenced by every triplet; real
  per-triplet binaries are a later CI phase (see
  `public/packages/README.md`).
- **Env for registry work:** `FORGE_REGISTRY_URL` (required by the CLI),
  `FORGE_ALLOW_UNSAFE_REGISTRY=1` (local `file://` registries/tests).
- **After touching `forge/`:** rebuild + rerun the vendored suite, and
  remember the copy flows upstream → sunn, never the reverse. See
  `forge/SOURCE.md`.
- **Commits:** conventional style (`feat:`, `fix:`, `docs:`), short
  subjects. Never commit `forge/build/`, `forge/target/`, `.next/`,
  or `node_modules/` (all ignored).
- **Releases (`@v1dxu/sunn`, one package, manual semver):** bump
  `version` in `package.json` → commit → tag `vX.Y.Z` → push the tag.
  The `publish` workflow re-runs every gate (frozen install, tsc,
  lint, build, registry drift check), refuses mismatched tag/version,
  then `npm publish --provenance`. First-ever publish is manual
  (`npm publish --access public` from the `v1dxu` login — the package
  must exist before npm lets you register it as a trusted publisher);
  after that OIDC handles it. Never commit npm tokens.

## Gotchas already learned here (don't rediscover)

- forge's own Makefile tracks headers (`-MMD -MP`); still, after header
  changes prefer `make clean && make CC=gcc` — stale objects crash
  opaquely (exit `0xC0000005`, buffered output lost).
- On MSYS2/Git-Bash, native System32 curl/tar must precede MSYS ones on
  PATH: MSYS `tar` cannot run as a grandchild of a native process (its
  gzip helper will not spawn). The regression script handles this; plain
  shells need it too.
- `bun run registry:build` output (`public/r/`) is committed; regenerate it
  when `registry/` changes.
- `bun run dev` is webpack (no `--turbopack`): Turbopack dev hard-fails
  parsing `tw-animate-css` (Tailwind v4 syntax in `@import`) under the v3
  pipeline — reproduced identically on Node, so it is a repo issue, not a
  Bun issue. Re-add the flag only after that import is resolved.
