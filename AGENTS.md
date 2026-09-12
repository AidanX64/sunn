# AGENTS.md — sunn

## Project overview

sunn is a self-hostable hybrid developer registry and the home of the Forge ecosystem.

It combines two complementary package ecosystems:

- A shadcn-style source registry for web development.
- A vcpkg/Conan-style native package registry for C, C++, and assembly.

The long-term goal is for both ecosystems to be provided by Sunn while preserving their different installation models.

### Web ecosystem

The web registry contains source-oriented developer building blocks:

- React components
- shadcn/ui components
- hooks
- utilities
- shaders
- WebGPU/WebGL components
- templates
- other reusable frontend source

The intended web installation experience is:

```sh
bunx sunn@latest add button
bunx sunn@latest add shader/aurora
bunx sunn@latest add command-palette
```

Web registry items follow the shadcn philosophy: source is copied into the user's project so it can be inspected, modified, and owned by the user.

The Sunn web CLI runs through Bun/Bunx and communicates with the Sunn web/source registry.

It is not a traditional npm package manager and should not turn registry items into opaque `node_modules` dependencies.

### Native ecosystem

The native registry contains reusable C, C++, and assembly packages.

The native Forge executable is responsible for native package management and build orchestration.

The intended native workflow is:

```sh
forge add fmt
forge add glfw
forge add my-library
```

Forge handles native concerns including:

- dependency resolution
- package versions
- lockfiles
- checksums
- source retrieval
- platform/triplet selection
- native builds
- package metadata
- future binary/artifact support

The native Forge implementation is written in C.

### Sunn and Forge ecosystem

There are two Sunn consumers with different runtimes:

```text
Web
  bunx sunn@latest add <item>
        │
        └── Sunn web/source registry
                │
                └── source assets

Native
  forge
        │
        └── Sunn native registry
                │
                └── C/C++/ASM packages
```

The shared concept is a unified Sunn ecosystem with separate web-source and native-package workflows.

Do not confuse this with making the web and native package formats identical. They have different semantics and should retain appropriate schemas and workflows.

## Repository architecture

```text
sunn/
├── app/                    Next.js web application
├── components/             React components
├── lib/                    shared web/registry functionality
├── registry/               shadcn-style web registry sources
├── public/packages/        native package registry
├── public/r/               built shadcn registry output
├── registry-fixtures/      native registry test fixtures
├── forge/                  vendored native Forge source
├── vendors/                future registry integrations
└── scripts/                registry/build/synchronization scripts
```

The Sunn web application is the registry frontend and server.

The Sunn web CLI is a Bun/TypeScript CLI distributed through the Bun/npm ecosystem.

The native Forge implementation remains the standalone C project vendored under `forge/`.

## Web registry

The web registry uses the shadcn registry format.

The existing shadcn registry files include:

```text
registry.json
registry/
public/r/
```

`registry.json` remains compatible with `shadcn build`.

Do not put native package definitions into the shadcn registry schema.

The Sunn web CLI should consume the registry in a way that preserves the shadcn source-oriented model.

A web registry item should be installable by name, for example:

```sh
bunx sunn@latest add button
bunx sunn@latest add shader/aurora
```

Names may use namespaces/categories such as:

```text
shader/aurora
shader/fluid
shader/noise
```

The CLI should resolve the requested item from the Sunn registry, retrieve its source, and place it into the appropriate project location according to the registry item's metadata.

Do not make web installation dependent on the native Forge binary.

## Native registry

The native registry is independent of the shadcn registry.

Native packages live under:

```text
public/packages/
```

and use the Sunn native registry schema validated by:

```text
lib/sunn-registry.ts
```

Forge currently resolves native packages through:

```text
GET /api/forge/v1/resolve?name=&version=
```

(`&triplet=` is reserved for the upcoming triplet phase; the client does
not send it yet.)

Preserve this contract unless there is a concrete reason to version or replace it.

The registry also serves a static `baseline.json` at the site root pinning
the minimum (version, revision) per package (vcpkg-baseline semantics).
Manifest entries use `version` for exact pins, `min-version` for minimums,
`version-range` for requirements (`= >= > <= < ^ ~`, wildcards, `,` AND,
`||` OR) with an optional `max-version` cap, or nothing (bare entries
track the baseline); top-level `[overrides]` force one exact version per
package. Recipes carry `revision` so a recipe fix ships without a new
upstream release. Requirements are validated by `lib/sunn-registry.ts`
(feature deps) and the forge manifest parser, exercised by
`forge/test/registry-regression.sh` (R2/R5/R10–R12 baseline pins,
R16–R19 ranges/max/overrides, R20 `FORGE_OVERLAYS` local-port shadowing,
R21 foreign-build arg validation) and `forge/test/deps-regression.sh`
(K8 foreign `make-args`/`make-target`).

Native fixture packages are currently source packages. They are not production binary releases.

Per-triplet binary artifacts are a future phase.

## Forge source of truth

`forge/` is a vendored copy of the standalone Forge repository
(`https://github.com/AidanX64/forge.git`, branch `master`).

The upstream Forge repository is the source of truth.

Never edit C sources directly inside the vendored `forge/` tree.

Forge changes must be made upstream, verified there, and then synchronized into Sunn using the existing synchronization workflow.

See:

```text
forge/AGENTS.md
forge/SOURCE.md
```

for Forge-specific rules.

## Technology stack

### Sunn web platform

Use the existing stack:

- Next.js
- React
- TypeScript
- Tailwind CSS
- shadcn/ui
- Bun

Do not migrate the web application to another framework without explicit approval.

Do not introduce Node.js as the primary JavaScript runtime when Bun can perform the task.

### Sunn web CLI

Use:

- Bun
- TypeScript

The CLI must be distributable through the Bun/npm ecosystem so that the intended usage is:

```sh
bunx sunn@latest add <item>
```

The web CLI should remain lightweight and focused on registry consumption.

### Native Forge

Use:

- C
- C++
- Assembly where appropriate

Follow `forge/AGENTS.md` for native implementation rules.

## Package-manager boundaries

Do not collapse the two package ecosystems into one generic package model.

Sunn web CLI:

```text
bunx sunn@latest add <source-item>
```

Native Forge:

```text
forge add <native-package>
```

The web side is source-oriented and shadcn-like.

The native side is dependency/build-oriented and vcpkg/Conan/Cargo-like.

Both are part of the Sunn/Forge ecosystem.

## Current implementation vs target implementation

Agents must distinguish between functionality that exists today and functionality that is part of the target architecture.

Currently, the published `sunn` package provides the Sunn registry/self-hosting CLI:

```sh
sunn serve
sunn init <dir>
sunn help
sunn --version
```

The native Forge implementation already provides native package-management functionality.

The Sunn web CLI and commands such as:

```sh
bunx sunn@latest add button
bunx sunn@latest add shader/aurora
```

are target functionality unless verified as implemented in the current repository.

Never document target functionality as currently working unless it has been implemented and tested.

## Development commands

Use Bun for the Sunn web project:

```sh
bun run dev
bun run build
bunx tsc --noEmit
bun run registry:build
bun run packages:fixtures
```

Native Forge:

```sh
bun run forge:build
bun run forge:test
bun run forge:clean
```

Synchronizing Forge:

```sh
bun run sync:forge
```

Use Bun for repository development and dependency installation.

## General implementation rules

Before making architectural changes:

1. Read the relevant existing implementation.
2. Determine what already works.
3. Preserve working behavior.
4. Avoid speculative abstractions.
5. Keep web and native registry schemas separate.
6. Do not invent undocumented behavior.
7. Add tests for new functionality.
8. Update documentation when CLI behavior changes.
9. Verify commands before claiming they work.

Prefer incremental implementation over broad rewrites.

The goal is not to clone shadcn, vcpkg, Conan, or Cargo.

The goal is to combine their strongest ideas into a unified Sunn/Forge ecosystem with a clean developer experience.
