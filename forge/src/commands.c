#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "forge/build.h"
#include "forge/compiler.h"
#include "forge/debug.h"
#include "forge/deps.h"
#include "forge/log.h"
#include "forge/manifest.h"
#include "forge/orchestrator.h"
#include "forge/pkg.h"
#include "forge/scaffold.h"

#define FORGE_PATH_MAX 1024U

/* Heap, not stack: ForgeManifest is ~360KB and these frames nest the
 * resolver (graph + another manifest + materialization frames), which used
 * to overflow the Windows stack mid-resolve. */
static ForgeManifest *alloc_manifest(void)
{
    ForgeManifest *manifest = calloc(1U, sizeof(*manifest));

    if (manifest == NULL) {
        fprintf(stderr, "forge: out of memory\n");
    }
    return manifest;
}

static int load_invocation(const char *manifest_path, const char *kind, ForgeLogger *logger,
                           ForgeManifest *manifest, char *root, size_t root_size)
{
    char error[FORGE_COMMAND_MAX];

    if (forge_build_project_root(manifest_path, root, root_size) != 0 ||
        forge_logger_init_in(logger, root, kind, error, sizeof(error)) != 0) {
        fprintf(stderr, "forge: %s\n", error);
        return -1;
    }
    forge_build_set_logger(logger);
    forge_log_set_session_logger(logger);
    forge_logger_detail(logger, kind, "log: %s", logger->path);
    forge_logger_detail(logger, "parse", "----- parse manifest %s -----", manifest_path);
    if (forge_manifest_load(manifest_path, manifest, error, sizeof(error)) != 0) {
        forge_logger_error(logger, "parse", "%s", error);
        return -1;
    }
    return 0;
}

static void finish_invocation(ForgeLogger *logger)
{
    forge_logger_close(logger);
    forge_build_set_logger(NULL);
    forge_log_set_session_logger(NULL);
}

int forge_orchestrate_run(const char *manifest_path, const ForgeBuildOptions *options,
                          const char *const *program_arguments,
                           size_t program_argument_count)
{
    ForgeManifest *manifest = alloc_manifest();
    ForgeLogger logger = {0};
    ForgeBuildOptions default_options = forge_build_default_options();
    const ForgeBuildOptions *effective = options != NULL ? options : &default_options;
    char root[FORGE_PATH_MAX];
    int child_exit_code = 0;
    int status;

    if (manifest == NULL) {
        return 1;
    }
    if (load_invocation(manifest_path, "build", &logger, manifest, root, sizeof(root)) != 0) {
        free(manifest);
        finish_invocation(&logger);
        return 1;
    }
    status = forge_build_project(root, manifest, manifest_path, FORGE_BUILD_MODE_RUN,
                                 effective, program_arguments,
                                 program_argument_count, NULL, 0U, &child_exit_code);
    forge_logger_detail(&logger, "build", "result: %s",
                        status == 0 ? "success" : "failed");
    finish_invocation(&logger);
    free(manifest);
    /* A successful pipeline propagates the program's own exit code. */
    return status != 0 ? 1 : child_exit_code;
}

int forge_orchestrate_build(const char *manifest_path, const ForgeBuildOptions *options)
{
    ForgeManifest *manifest = alloc_manifest();
    ForgeLogger logger = {0};
    ForgeBuildOptions default_options = forge_build_default_options();
    const ForgeBuildOptions *effective = options != NULL ? options : &default_options;
    char root[FORGE_PATH_MAX];
    char executable[FORGE_PATH_MAX];
    int result;

    if (manifest == NULL) {
        return 1;
    }
    if (load_invocation(manifest_path, "build", &logger, manifest, root, sizeof(root)) != 0) {
        free(manifest);
        finish_invocation(&logger);
        return 1;
    }
    result = forge_build_project(root, manifest, manifest_path, FORGE_BUILD_MODE_LINK,
                                 effective, NULL, 0U, executable,
                                 sizeof(executable), NULL) == 0 ? 0 : 1;
    forge_logger_detail(&logger, "build", "result: %s",
                        result == 0 ? "success" : "failed");
    finish_invocation(&logger);
    free(manifest);
    return result;
}

int forge_orchestrate_check(const char *manifest_path, const ForgeBuildOptions *options)
{
    ForgeManifest *manifest = alloc_manifest();
    ForgeLogger logger = {0};
    ForgeBuildOptions default_options = forge_build_default_options();
    const ForgeBuildOptions *effective = options != NULL ? options : &default_options;
    char root[FORGE_PATH_MAX];
    int result;

    if (manifest == NULL) {
        return 1;
    }
    if (load_invocation(manifest_path, "check", &logger, manifest, root, sizeof(root)) != 0) {
        free(manifest);
        finish_invocation(&logger);
        return 1;
    }
    result = forge_build_project(root, manifest, manifest_path,
                                 FORGE_BUILD_MODE_COMPILE_ONLY, effective,
                                 NULL, 0U, NULL, 0U, NULL) == 0 ? 0 : 1;
    forge_logger_detail(&logger, "check", "result: %s",
                        result == 0 ? "success" : "failed");
    finish_invocation(&logger);
    free(manifest);
    return result;
}

int forge_orchestrate_test(const char *manifest_path, const ForgeBuildOptions *options,
                           const char *test_filter)
{
    ForgeManifest *manifest = alloc_manifest();
    ForgeLogger logger = {0};
    ForgeBuildOptions default_options = forge_build_default_options();
    const ForgeBuildOptions *effective = options != NULL ? options : &default_options;
    char root[FORGE_PATH_MAX];
    int result;

    if (manifest == NULL) {
        return 1;
    }
    if (load_invocation(manifest_path, "test", &logger, manifest, root, sizeof(root)) != 0) {
        free(manifest);
        finish_invocation(&logger);
        return 1;
    }
    result = forge_build_tests(root, manifest, manifest_path, effective,
                               test_filter);
    forge_logger_detail(&logger, "test", "result: %s",
                        result == 0 ? "success" : (result < 0 ? "failed" : "tests failed"));
    finish_invocation(&logger);
    free(manifest);
    return result < 0 ? 1 : result;
}

int forge_orchestrate_update(const char *manifest_path, const char *only_name,
                             int offline)
{
    ForgeManifest *manifest = alloc_manifest();
    ForgeLogger logger = {0};
    ForgeDepGraph *graph = calloc(1U, sizeof(*graph));
    char root[FORGE_PATH_MAX];
    char error[FORGE_COMMAND_MAX] = {0};
    size_t index;
    int known = 0;
    int result;
    const char *resolve_name = NULL;

    if (manifest == NULL || graph == NULL) {
        free(manifest);
        free(graph);
        fprintf(stderr, "forge: out of memory\n");
        return 1;
    }
    if (load_invocation(manifest_path, "update", &logger, manifest, root, sizeof(root)) != 0) {
        free(manifest);
        free(graph);
        finish_invocation(&logger);
        return 1;
    }
    if (manifest->dependencies.count == 0U && only_name != NULL) {
        forge_logger_error(&logger, "update",
                           "dependency '%s' is not declared in [dependencies]",
                           only_name);
        free(manifest);
        free(graph);
        finish_invocation(&logger);
        return 1;
    }
    if (manifest->dependencies.count == 0U) {
        forge_logger_log(&logger, "update", "no [dependencies] to update");
        free(manifest);
        free(graph);
        finish_invocation(&logger);
        return 0;
    }
    if (only_name != NULL) {
        for (index = 0U; index < manifest->dependencies.count; ++index) {
            if (strcmp(manifest->dependencies.items[index].name, only_name) == 0) {
                known = 1;
                resolve_name = manifest->dependencies.items[index].path[0] == '\0'
                                   ? only_name
                                   : NULL;
            }
        }
        if (!known) {
            forge_logger_error(&logger, "update",
                               "dependency '%s' is not declared in [dependencies]; "
                               "declared dependencies:", only_name);
            for (index = 0U; index < manifest->dependencies.count; ++index) {
                forge_logger_log(&logger, "update", "  %s",
                                 manifest->dependencies.items[index].name);
            }
            free(manifest);
            free(graph);
            finish_invocation(&logger);
            return 1;
        }
        if (resolve_name == NULL) {
            /*
             * Path deps are used in place and have nothing to re-resolve.
             * Return instead of falling through: with a NULL name the shared
             * resolver treats this as a bare update and forces every git pin,
             * which is exactly what naming one dependency must never do.
             */
            forge_logger_log(&logger, "update",
                             "'%s' is a path dependency; nothing to update", only_name);
            free(manifest);
            free(graph);
            finish_invocation(&logger);
            return 0;
        }
    }
    /*
     * Bare `update` forces every dependency past its pin (the documented
     * "re-resolve all" behavior); naming one restricts the force to that
     * dependency alone via force_update_name.
     */
    result = forge_deps_resolve(root, manifest, resolve_name == NULL ? 1 : 0,
                                resolve_name != NULL ? resolve_name : "",
                                offline, 0, graph,
                                &logger, error, sizeof(error)) == 0 ? 0 : 1;
    if (result != 0 && error[0] != '\0') {
        forge_logger_error(&logger, "update", "%s", error);
    } else {
        forge_logger_log(&logger, "update", "resolved %zu dependencies",
                         graph->count);
    }
    forge_deps_free_graph(graph);
    forge_logger_detail(&logger, "update", "result: %s",
                        result == 0 ? "success" : "failed");
    finish_invocation(&logger);
    free(manifest);
    free(graph);
    return result;
}

int forge_orchestrate_debug(const char *manifest_path, const ForgeBuildOptions *options)
{
    ForgeManifest *manifest = alloc_manifest();
    ForgeLogger logger = {0};
    ForgeBuildOptions default_options = forge_build_default_options();
    const ForgeBuildOptions *effective = options != NULL ? options : &default_options;
    char error[FORGE_COMMAND_MAX] = {0};
    char root[FORGE_PATH_MAX];
    char executable[FORGE_PATH_MAX];
    int result;

    if (manifest == NULL) {
        return 1;
    }
    if (load_invocation(manifest_path, "debug", &logger, manifest, root, sizeof(root)) != 0) {
        free(manifest);
        finish_invocation(&logger);
        return 1;
    }
    result = forge_build_project(root, manifest, manifest_path, FORGE_BUILD_MODE_LINK,
                                 effective, NULL, 0U, executable,
                                 sizeof(executable), NULL) == 0 &&
             forge_debug_launch(executable, &logger, error, sizeof(error)) == 0 ? 0 : 1;
    if (result != 0 && error[0] != '\0') {
        forge_logger_error(&logger, "debug", "%s", error);
    }
    forge_logger_detail(&logger, "debug", "result: %s",
                        result == 0 ? "success" : "failed");
    finish_invocation(&logger);
    free(manifest);
    return result;
}

int forge_orchestrate_clean(const char *manifest_path)
{
    return forge_build_clean(manifest_path);
}

int forge_orchestrate_add(const char *manifest_path, const char *name,
                          const char *git_url, const char *ref_kind,
                          const char *ref_value, const char *dep_path,
                          const char *registry_package,
                          const char *registry_version,
                          const char *registry_min_version,
                          const char *registry_range,
                          const char *registry_max_version,
                          const char *registry_features,
                          int registry_no_default_features)
{
    ForgeLogger logger = {0};
    char error[FORGE_COMMAND_MAX] = {0};
    char root[FORGE_PATH_MAX];
    int result = 0;

    if (forge_build_project_root(manifest_path, root, sizeof(root)) != 0) {
        return 1;
    }
    if (forge_logger_init_in(&logger, root, "deps", error, sizeof(error)) != 0) {
        fprintf(stderr, "forge: %s\n", error);
        return 1;
    }
    forge_build_set_logger(&logger);
    forge_log_set_session_logger(&logger);
    if (forge_pkg_add(manifest_path, name, git_url, ref_kind, ref_value, dep_path,
                      registry_package, registry_version, registry_min_version,
                      registry_range, registry_max_version,
                      registry_features, registry_no_default_features,
                      &logger, error, sizeof(error)) != 0) {
        forge_logger_error(&logger, "deps", "%s", error);
        result = 1;
    }
    forge_logger_close(&logger);
    forge_build_set_logger(NULL);
    forge_log_set_session_logger(NULL);
    return result;
}

int forge_orchestrate_remove(const char *manifest_path, const char *name)
{
    ForgeLogger logger = {0};
    char error[FORGE_COMMAND_MAX] = {0};
    char root[FORGE_PATH_MAX];
    int result = 0;

    if (forge_build_project_root(manifest_path, root, sizeof(root)) != 0) {
        return 1;
    }
    if (forge_logger_init_in(&logger, root, "deps", error, sizeof(error)) != 0) {
        fprintf(stderr, "forge: %s\n", error);
        return 1;
    }
    forge_build_set_logger(&logger);
    forge_log_set_session_logger(&logger);
    if (forge_pkg_remove(manifest_path, name, &logger, error, sizeof(error)) != 0) {
        forge_logger_error(&logger, "deps", "%s", error);
        result = 1;
    }
    forge_logger_close(&logger);
    forge_build_set_logger(NULL);
    forge_log_set_session_logger(NULL);
    return result;
}

int forge_orchestrate_new(const char *name)
{
    char error[FORGE_COMMAND_MAX] = {0};

    if (forge_scaffold_new_project(name, error, sizeof(error)) != 0) {
        fprintf(stderr, "forge: %s\n", error);
        return 1;
    }
    printf("Created new project '%s' (Forge.toml, src/main.c)\n"
           "Run it with: cd %s && forge run\n", name, name);
    return 0;
}

int forge_orchestrate_init(void)
{
    char error[FORGE_COMMAND_MAX] = {0};

    if (forge_scaffold_init_project(error, sizeof(error)) != 0) {
        fprintf(stderr, "forge: %s\n", error);
        return 1;
    }
    printf("Initialized forge project in the current directory\n"
           "Run it with: forge run\n");
    return 0;
}
