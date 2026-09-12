#ifndef FORGE_ORCHESTRATOR_H
#define FORGE_ORCHESTRATOR_H

#include "forge/build.h"

/* Builds the current project's manifest target without running it. */
int forge_orchestrate_build(const char *manifest_path, const ForgeBuildOptions *options);
/* Compiles every translation unit without linking (fast validation). */
int forge_orchestrate_check(const char *manifest_path, const ForgeBuildOptions *options);
/* Builds then executes the current project's manifest target; the trailing
 * arguments are passed to the program and its exit code becomes the return
 * value. */
int forge_orchestrate_run(const char *manifest_path, const ForgeBuildOptions *options,
                          const char *const *program_arguments,
                          size_t program_argument_count);
/* Builds and runs every standalone test source in tests/. When `test_filter`
 * is non-NULL only the test whose binary name matches it runs. */
int forge_orchestrate_test(const char *manifest_path, const ForgeBuildOptions *options,
                           const char *test_filter);
/* Re-resolves [dependencies] refs and rewrites Forge.lock. When `only_name`
 * is non-NULL only that dependency is re-resolved; other lock pins stay.
 * `offline` forbids network access (cached pins still resolve). */
int forge_orchestrate_update(const char *manifest_path, const char *only_name,
                             int offline);
/* Adds a [dependencies] entry (git, path, or registry source) and
 * re-resolves pins. `registry_version` pins one release exactly;
 * `registry_min_version` sets a minimum, `registry_range` sets a
 * version-range requirement, `registry_max_version` sets an inclusive
 * upper bound ("" for all floats tracks the registry baseline, newest
 * when the registry states none — the resolved release is still pinned
 * in Forge.lock immediately). At most one of version/min-version/range
 * may be non-empty, max-version never combines with exact version, and
 * features need a registry source. */
int forge_orchestrate_add(const char *manifest_path, const char *name,
                          const char *git_url, const char *ref_kind,
                          const char *ref_value, const char *dep_path,
                          const char *registry_package,
                          const char *registry_version,
                          const char *registry_min_version,
                          const char *registry_range,
                          const char *registry_max_version,
                          const char *registry_features,
                          int registry_no_default_features);
/* Removes a [dependencies] entry and prunes its pin from Forge.lock. */
int forge_orchestrate_remove(const char *manifest_path, const char *name);
int forge_orchestrate_debug(const char *manifest_path, const ForgeBuildOptions *options);
int forge_orchestrate_clean(const char *manifest_path);
/* Scaffolds a new project directory named `name` / initializes the cwd. */
int forge_orchestrate_new(const char *name);
int forge_orchestrate_init(void);

#endif
