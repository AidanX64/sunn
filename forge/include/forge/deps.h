#ifndef FORGE_DEPS_H
#define FORGE_DEPS_H

#include <stddef.h>

#include "forge/compiler.h"
#include "forge/log.h"
#include "forge/manifest.h"
#include "forge/paths.h"

/*
 * Shared dependency cache root: $FORGE_HOME, or ~/.forge when unset
 * (USERPROFILE on Windows). Git checkouts live under <root>/git,
 * registry packages under <root>/registry. Returns 0 on success.
 */
int forge_deps_cache_home(char *destination, size_t destination_size);

#define FORGE_DEPS_MAX_NODES 128U
#define FORGE_DEPS_MAX_DEPTH 32U

/*
 * One resolved node of the dependency graph. `manifest` is heap-allocated
 * and only valid for native dependencies (those with their own Forge.toml).
 * Nodes are stored in build order: a dependency always appears before the
 * projects that use it; iterate in reverse for linker input ordering.
 */
typedef struct ForgeDepNode {
    char name[FORGE_MANIFEST_VALUE_MAX];
    char root[FORGE_PATH_MAX];
    /* Source identity, kept so two declarations of the same dependency name
     * with different sources fail loudly instead of first-one-wins. For git
     * deps `source_url`/`source_ref` hold the manifest strings and
     * `source_root` is empty; for registry deps they hold the package name
     * and pinned version (`is_registry` set); for path deps `source_root`
     * holds the canonicalized directory and the other two are empty. */
    char source_url[FORGE_MANIFEST_VALUE_MAX];
    char source_ref[FORGE_MANIFEST_VALUE_MAX];
    char source_path[FORGE_PATH_MAX];
    int is_registry;
    ForgeManifest *manifest;
    int is_native;
    /* Filled by the build stage: native deps point at their objects.txt
     * listing, foreign deps at the static library their build produced. */
    char link_artifact[FORGE_PATH_MAX];
} ForgeDepNode;

typedef struct ForgeDepGraph {
    ForgeDepNode nodes[FORGE_DEPS_MAX_NODES];
    size_t count;
} ForgeDepGraph;

/*
 * Decides whether a manifest-supplied git URL is safe to hand to
 * `git clone` verbatim: https://, ssh://, and scp-style git@host:path are
 * allowed; everything else (ext::/fd:: transports that execute shell,
 * file://, bare local paths, option-shaped "-..." strings) is rejected.
 * FORGE_ALLOW_UNSAFE_GIT=1 in the environment lifts the restriction for
 * local testing. Returns 0 when the URL is supported, -1 otherwise with a
 * human-readable reason in `error`.
 */
int forge_deps_git_url_is_supported(const char *url, char *error, size_t error_size);

/*
 * Resolves the manifest's [dependencies] transitively: fetches git deps into
 * the shared cache (~/.forge/git), checks out locked commits, records them in
 * <project_root>/Forge.lock, parses each dependency's own manifest, and
 * rejects dependency cycles. `force_update` re-resolves refs even when the
 * lockfile already matches; when it is zero, `force_update_name` (NULL or
 * empty for "none") limits that re-resolution to the one named dependency,
 * leaving every other lock pin untouched. With `offline` set no network is
 * used at all: a cached clone with a valid lock pin still resolves, anything
 * needing clone/fetch fails with a readable error. With `locked` set a
 * resolution that would move or add a pin fails instead of rewriting
 * Forge.lock. Returns 0 on success.
 */
int forge_deps_resolve(const char *project_root, const ForgeManifest *manifest,
                       int force_update, const char *force_update_name,
                       int offline, int locked,
                       ForgeDepGraph *graph, ForgeLogger *logger,
                       char *error, size_t error_size);

/* Releases the per-node manifests owned by the graph. */
void forge_deps_free_graph(ForgeDepGraph *graph);

/*
 * Collects one include directory per node (<root>/include when present,
 * otherwise the root itself) into `output`. Returns 0 on success.
 */
int forge_deps_include_dirs(const ForgeDepGraph *graph, ForgeStringList *output,
                            char *error, size_t error_size);

/*
 * Builds a foreign dependency with its own build system (CMake or Make) and
 * locates the produced static library. Native dependencies are built by the
 * orchestrator instead. Returns 0 with the artifact path in `artifact`.
 */
int forge_deps_build_foreign(const ForgeDepNode *node, const ForgeCompiler *compiler,
                             int release, int max_jobs, ForgeLogger *logger,
                             char *artifact, size_t artifact_size,
                             char *error, size_t error_size);

#endif
