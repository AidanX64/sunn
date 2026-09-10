#ifndef FORGE_BUILD_H
#define FORGE_BUILD_H

#include <stddef.h>

#include "forge/log.h"
#include "forge/manifest.h"

/* What should happen once every translation unit is compiled. */
typedef enum ForgeBuildMode {
    FORGE_BUILD_MODE_LINK,      /* compile then link (forge build) */
    FORGE_BUILD_MODE_RUN,       /* compile, link, execute (forge run/test) */
    FORGE_BUILD_MODE_COMPILE_ONLY, /* compile without linking (forge check) */
    /* Internal: compile a dependency project and record its object paths in
     * <profile>/objects.txt; `built_executable` receives that file's path. */
    FORGE_BUILD_MODE_DEP_OBJECTS
} ForgeBuildMode;

/*
 * Invocation settings shared by every build-style command, threaded from the
 * CLI through the orchestrators into the engine and dependency resolution.
 */
typedef struct ForgeBuildOptions {
    int release;  /* build the [profile.release] variant */
    int max_jobs; /* compile parallelism; 0 = one job per logical processor */
    int offline;  /* --offline: resolving dependencies must not use network */
    int locked;   /* --locked: resolution must not change any Forge.lock pin */
} ForgeBuildOptions;

/* Defaults for a plain `forge build`-style invocation (debug, auto jobs). */
ForgeBuildOptions forge_build_default_options(void);

/* The build engine operates on a parsed manifest and an invocation logger. */
void forge_build_set_logger(ForgeLogger *logger);
int forge_build_project_root(const char *manifest_path, char *root, size_t root_size);

/*
 * Builds the project described by `manifest` in `project_root`.
 *
 * `program_arguments` (only meaningful for FORGE_BUILD_MODE_RUN) are appended
 * after the executable when spawning it. When `child_exit_code` is not NULL it
 * receives the spawned program's exit code; the function's own return value
 * stays 0/-1 for "the build pipeline worked / failed", so callers decide
 * whether a nonzero child code becomes their exit status.
 * `manifest_path` anchors link-level freshness (the executable is not
 * relinked while its recorded inputs are unchanged); pass NULL to opt out.
 */
int forge_build_project(const char *project_root, const ForgeManifest *manifest,
                        const char *manifest_path,
                        ForgeBuildMode mode, const ForgeBuildOptions *options,
                        const char *const *program_arguments,
                        size_t program_argument_count,
                        char *built_executable, size_t built_executable_size,
                        int *child_exit_code);
int forge_build_clean(const char *manifest_path);

/*
 * Builds and runs every standalone test source in <root>/tests (each test
 * file is its own binary with its own main). When `test_filter` is non-NULL
 * only the test whose binary name equals it runs. Returns 0 when all passed,
 * 1 when any failed.
 */
int forge_build_tests(const char *project_root, const ForgeManifest *manifest,
                      const char *manifest_path,
                      const ForgeBuildOptions *options, const char *test_filter);

#endif
