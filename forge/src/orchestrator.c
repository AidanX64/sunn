#include "forge/platform.h"

#if !FORGE_PLATFORM_WINDOWS
#define _XOPEN_SOURCE 700
#define _POSIX_C_SOURCE 200809L
#endif

#ifdef _MSC_VER
#define _CRT_SECURE_NO_WARNINGS
#endif

#include <ctype.h>
#include <errno.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#if FORGE_PLATFORM_WINDOWS
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#else
#include <dirent.h>
#include <sys/types.h>
#include <unistd.h>
#endif
#include <sys/stat.h>

#include "forge/argv.h"
#include "forge/compiler.h"
#include "forge/debug.h"
#include "forge/build.h"
#include "forge/deps.h"
#include "forge/fingerprint.h"
#include "forge/log.h"
#include "forge/manifest.h"
#include "forge/orchestrator.h"
#include "forge/paths.h"
#include "forge/process.h"
#include "forge/sources.h"
#include "forge/thread.h"
#include "forge_util.h"

static ForgeLogger *active_logger;

/* Growable list of dependency object/library paths fed to the linker. */
typedef struct ForgePathList {
    char **items;
    size_t count;
    size_t capacity;
} ForgePathList;

static char *path_list_copy(const char *path)
{
    size_t length = strlen(path);
    char *copy = malloc(length + 1U);

    if (copy != NULL) {
        memcpy(copy, path, length + 1U);
    }
    return copy;
}

static int path_list_push(ForgePathList *list, const char *path)
{
    if (list->count == list->capacity) {
        size_t new_capacity = list->capacity == 0U ? 16U : list->capacity * 2U;
        char **grown = realloc(list->items, new_capacity * sizeof(*grown));

        if (grown == NULL) {
            return -1;
        }
        list->items = grown;
        list->capacity = new_capacity;
    }
    list->items[list->count] = path_list_copy(path);
    if (list->items[list->count] == NULL) {
        return -1;
    }
    ++list->count;
    return 0;
}

static void path_list_free(ForgePathList *list)
{
    size_t index;

    for (index = 0U; index < list->count; ++index) {
        free(list->items[index]);
    }
    free(list->items);
    list->items = NULL;
    list->count = 0U;
    list->capacity = 0U;
}

/* Reads one path per line (the objects.txt a DEP_OBJECTS build wrote). */
static int read_path_list(const char *file_path, ForgePathList *list)
{
    FILE *file = fopen(file_path, "r");
    char line[FORGE_PATH_MAX];

    if (file == NULL) {
        return 0; /* no recorded objects is not an error */
    }
    while (fgets(line, sizeof(line), file) != NULL) {
        line[strcspn(line, "\r\n")] = '\0';
        if (line[0] == '\0') {
            continue;
        }
        if (path_list_push(list, line) != 0) {
            (void)fclose(file);
            return -1;
        }
    }
    (void)fclose(file);
    return 0;
}

/*
 * Link-level freshness. After a successful link, forge records every input
 * state — each object's and library's exact mtime plus the manifest's — in a
 * small stamp file next to the executable. A later run relinks only when the
 * executable is missing or the recorded states no longer match, which is the
 * same contract Cargo's fingerprint provides: source edits change mtimes,
 * adding/removing sources changes the list itself, and profile edits touch
 * the manifest.
 */

typedef struct ForgeTextBuilder {
    char *text;
    size_t length;
    size_t capacity;
} ForgeTextBuilder;

static int text_builder_add(ForgeTextBuilder *builder, const char *line)
{
    size_t line_length = strlen(line);

    if (builder->length + line_length + 1U > builder->capacity) {
        size_t new_capacity = builder->capacity == 0U ? 4096U : builder->capacity;
        char *grown;

        while (builder->length + line_length + 1U > new_capacity) {
            if (new_capacity > SIZE_MAX / 2U) {
                return -1;
            }
            new_capacity *= 2U;
        }
        grown = realloc(builder->text, new_capacity);
        if (grown == NULL) {
            return -1;
        }
        builder->text = grown;
        builder->capacity = new_capacity;
    }
    memcpy(builder->text + builder->length, line, line_length);
    builder->length += line_length;
    builder->text[builder->length] = '\0';
    return 0;
}

static void text_builder_free(ForgeTextBuilder *builder)
{
    free(builder->text);
    builder->text = NULL;
    builder->length = 0U;
    builder->capacity = 0U;
}

/* Appends "kind<TAB>path<TAB>mtime\n" for one link input. Paths are recorded
 * fully normalized so the stamp text never depends on how forge was invoked
 * (a relative --manifest argument vs upward discovery from a subdirectory). */
static int stamp_add_input(ForgeTextBuilder *builder, const char *kind,
                           const char *path)
{
    char mtime[64];
    char absolute[FORGE_PATH_MAX];
    char line[FORGE_PATH_MAX + 96U];

    if (forge_paths_absolute(path, absolute, sizeof(absolute)) != 0) {
        (void)snprintf(absolute, sizeof(absolute), "%s", path);
    }
    forge_fingerprint_mtime_text(absolute, mtime, sizeof(mtime));
    if ((size_t)snprintf(line, sizeof(line), "%s\t%s\t%s\n", kind, absolute,
                         mtime) >= sizeof(line)) {
        return -1;
    }
    return text_builder_add(builder, line);
}

static int read_whole_file_text(const char *path, char **contents, size_t *size)
{
    FILE *file = fopen(path, "rb");
    long length;

    *contents = NULL;
    *size = 0U;
    if (file == NULL) {
        return -1;
    }
    if (fseek(file, 0L, SEEK_END) != 0 || (length = ftell(file)) < 0L ||
        fseek(file, 0L, SEEK_SET) != 0) {
        (void)fclose(file);
        return -1;
    }
    /* An empty stamp is never valid, so treat it as absent. */
    if (length == 0L) {
        (void)fclose(file);
        return -1;
    }
    *contents = malloc((size_t)length + 1U);
    if (*contents == NULL) {
        (void)fclose(file);
        return -1;
    }
    *size = fread(*contents, 1U, (size_t)length, file);
    (void)fclose(file);
    (*contents)[*size] = '\0';
    return 0;
}

/*
 * Builds the exact stamp text describing this set of link inputs. Always
 * called before deciding anything, so a fresh link can record what it built
 * even on the very first build (when there is nothing to compare against).
 */
static int build_link_expected(ForgeTextBuilder *expected,
                               const char *manifest_path_for_freshness,
                               const char *const *object_paths, size_t object_count,
                               const char *const *extra_inputs, size_t extra_count)
{
    memset(expected, 0, sizeof(*expected));
    if (text_builder_add(expected, "forge-link-stamp v1\n") != 0) {
        return -1;
    }
    if (manifest_path_for_freshness != NULL &&
        stamp_add_input(expected, "manifest", manifest_path_for_freshness) != 0) {
        return -1;
    }
    for (size_t index = 0U; index < object_count; ++index) {
        if (object_paths[index] != NULL &&
            stamp_add_input(expected, "object", object_paths[index]) != 0) {
            return -1;
        }
    }
    for (size_t index = 0U; index < extra_count; ++index) {
        if (extra_inputs[index] != NULL && extra_inputs[index][0] != '\0' &&
            stamp_add_input(expected, "library", extra_inputs[index]) != 0) {
            return -1;
        }
    }
    return 0;
}

/*
 * Returns 1 when `executable_path` exists and the stamp beside it still
 * describes exactly the inputs captured in `expected`.
 */
static int link_is_fresh(const char *executable_path, const char *stamp_path,
                         const ForgeTextBuilder *expected,
                         struct stat *executable_stat)
{
    char *recorded = NULL;
    size_t recorded_size = 0U;
    int fresh = 0;

    if (stat(executable_path, executable_stat) != 0 ||
        !S_ISREG(executable_stat->st_mode)) {
        return 0;
    }
    if (read_whole_file_text(stamp_path, &recorded, &recorded_size) == 0) {
        fresh = recorded_size == expected->length &&
                memcmp(recorded, expected->text, recorded_size) == 0;
        free(recorded);
    }
    return fresh;
}

static void write_link_stamp(const char *stamp_path, const ForgeTextBuilder *expected,
                             const char *executable_name)
{
    FILE *file = fopen(stamp_path, "wb");

    if (file == NULL) {
        /* Without a stamp the next build simply relinks once more. */
        forge_logger_log(active_logger, "link",
                         "note: could not record the link stamp for %s "
                         "(future runs will relink)", executable_name);
        return;
    }
    if (fwrite(expected->text, 1U, expected->length, file) != expected->length) {
        forge_logger_log(active_logger, "link",
                         "note: could not finish the link stamp for %s",
                         executable_name);
    }
    (void)fclose(file);
}

static const char *log_capture_path(void)
{
    return (active_logger != NULL && active_logger->path[0] != '\0')
               ? active_logger->path
               : NULL;
}

static void print_error(const char *format, ...)
{
    va_list arguments;

    va_start(arguments, format);
    if (active_logger != NULL) {
        char message[FORGE_COMMAND_MAX];
        (void)vsnprintf(message, sizeof(message), format, arguments);
        forge_logger_error(active_logger, "build", "%s", message);
    } else {
        fprintf(stderr, "forge: ");
        (void)vfprintf(stderr, format, arguments);
        fputc('\n', stderr);
    }
    va_end(arguments);
}

/*
 * Emits the tail of the invocation log to stderr. Stage output is appended to
 * the log file during a build, so on a failed stage the terminal should show
 * the real diagnostic instead of only a terse "failed" line.
 */
static void print_log_tail(size_t max_lines)
{
    FILE *file;
    char *contents;
    char *start;
    char *cursor;
    size_t size;
    size_t lines = 0U;

    if (active_logger == NULL || active_logger->path[0] == '\0') {
        return;
    }
    file = fopen(active_logger->path, "rb");
    if (file == NULL) {
        fprintf(stderr, "forge: [error] could not read the build log at %s\n",
                active_logger->path);
        return;
    }
    if (fseek(file, 0L, SEEK_END) != 0) {
        (void)fclose(file);
        return;
    }
    {
        long length = ftell(file);
        if (length <= 0L) {
            (void)fclose(file);
            return;
        }
        contents = malloc((size_t)length + 1U);
        if (contents == NULL) {
            (void)fclose(file);
            return;
        }
        rewind(file);
        size = fread(contents, 1U, (size_t)length, file);
        (void)fclose(file);
        contents[size] = '\0';
    }
    cursor = contents + size;
    while (cursor > contents && lines < max_lines) {
        char *previous = cursor - 1;
        while (previous > contents && previous[-1] != '\n') {
            --previous;
        }
        if (previous == contents) {
            start = previous;
            ++lines;
            break;
        }
        start = previous;
        ++lines;
        cursor = previous - 1;
    }
    fprintf(stderr, "forge: [error] last %zu log line(s):\n", lines);
    {
        char *line = start;
        while (line != NULL && line < contents + size) {
            char *end = strchr(line, '\n');
            size_t length = end == NULL ? (size_t)(contents + size - line)
                                        : (size_t)(end - line);
            while (length > 0U && (line[length - 1U] == '\r' ||
                                   line[length - 1U] == '\n')) {
                --length;
            }
            fprintf(stderr, "  %.*s\n", (int)length, line);
            line = end == NULL ? NULL : end + 1;
        }
    }
    free(contents);
}

int forge_build_project_root(const char *manifest_path, char *root, size_t root_size)
{
    char error[FORGE_COMMAND_MAX] = {0};
    int status = forge_paths_project_root(manifest_path, root, root_size,
                                          error, sizeof(error));

    if (status != 0 && error[0] != '\0') {
        print_error("%s", error);
    }
    return status;
}

static const char *host_architecture(void)
{
#if defined(_M_X64) || defined(__x86_64__) || defined(__amd64__)
    return "x86_64";
#elif defined(_M_IX86) || defined(__i386__)
    return "x86";
#elif defined(_M_ARM64) || defined(__aarch64__)
    return "aarch64";
#elif defined(_M_ARM) || defined(__arm__)
    return "arm";
#else
    return "unknown";
#endif
}

static int list_contains(const ForgeStringList *list, const char *value)
{
    size_t index;
    for (index = 0U; index < list->count; ++index) {
        if (strcmp(list->items[index], value) == 0) {
            return 1;
        }
    }
    return 0;
}

/* Copies " v1.2.3" into `buffer` when the manifest declares a version, else
 * leaves it empty — the suffix appended to every Compiling/Checking line. */
static void version_suffix(const ForgeManifest *manifest, char *buffer,
                           size_t buffer_size)
{
    if (manifest != NULL && manifest->project_version[0] != '\0') {
        (void)snprintf(buffer, buffer_size, " v%s", manifest->project_version);
    } else {
        buffer[0] = '\0';
    }
}

static int select_host_target(const ForgeManifest *manifest, const ForgeHostInfo *host,
                              const char **target_os, const char **target_arch)
{
    const char *architecture = host_architecture();
    const char *os = forge_host_os_name(host->os);

    if (!list_contains(&manifest->target_os, os) ||
        !list_contains(&manifest->target_arch, architecture)) {
        print_error("manifest does not include this host target (%s/%s); "
                    "cross-compilation needs an explicit toolchain and is not configured",
                    os, architecture);
        return -1;
    }
    *target_os = os;
    *target_arch = architecture;
    return 0;
}

/* Body writer for the atomic objects.txt replacement below. */
typedef struct ForgeObjectListBody {
    const char **references;
    size_t count;
} ForgeObjectListBody;

static int write_object_list_body(void *user_data, FILE *file)
{
    const ForgeObjectListBody *body = user_data;
    size_t index;

    for (index = 0U; index < body->count; ++index) {
        if (fprintf(file, "%s\n", body->references[index]) < 0) {
            return -1;
        }
    }
    return 0;
}

/*
 * True when `child` belongs to the current build's live object set: either a
 * listed object itself or one of its twins — the `-MF` depfile
 * (`<hash>.o` <-> `<hash>.d`) and the compile-command stamp
 * (`<hash>.o.cmdhash`). Twins are never in the object list, but deleting one
 * would force a pointless recompile on every later build.
 */
static int object_artifact_is_used(const char *child,
                                   char (*used_paths)[FORGE_PATH_MAX],
                                   size_t used_count)
{
    size_t index;
    size_t length = strlen(child);
    const char *twin = NULL;
    char object_spelling[FORGE_PATH_MAX];

    for (index = 0U; index < used_count; ++index) {
        if (strcmp(used_paths[index], child) == 0) {
            return 1;
        }
    }
    if (forge_util_has_suffix(child, ".d")) {
        if (length + 1U >= sizeof(object_spelling)) {
            return 0;
        }
        (void)snprintf(object_spelling, sizeof(object_spelling), "%s", child);
        object_spelling[length - 1U] = 'o'; /* ".d" -> ".o" */
        twin = object_spelling;
    } else if (forge_util_has_suffix(child, ".cmdhash")) {
        size_t base_length = length - 8U; /* strlen(".cmdhash") */

        if (base_length == 0U || base_length + 1U > sizeof(object_spelling)) {
            return 0;
        }
        memcpy(object_spelling, child, base_length);
        object_spelling[base_length] = '\0';
        twin = object_spelling;
    }
    if (twin == NULL) {
        return 0;
    }
    for (index = 0U; index < used_count; ++index) {
        if (strcmp(used_paths[index], twin) == 0) {
            return 1;
        }
    }
    return 0;
}

/*
 * Deletes .o/.d files in `directory` that no current source produced, so a
 * renamed or deleted translation unit cannot keep feeding stale objects into
 * incremental link decisions. Best effort: failures (locked files, exotic
 * permissions) are counted and reported but never fail the build.
 */
static void remove_orphan_objects(const char *directory,
                                  char (*used_paths)[FORGE_PATH_MAX],
                                  size_t used_count)
{
#if FORGE_PLATFORM_WINDOWS
    WIN32_FIND_DATAA entry;
    HANDLE handle;
    char pattern[FORGE_PATH_MAX];
#else
    DIR *stream;
    struct dirent *entry;
#endif
    size_t removed = 0U;

    if (directory == NULL || used_paths == NULL) {
        return;
    }
#if FORGE_PLATFORM_WINDOWS
    if (forge_paths_join(pattern, sizeof(pattern), directory, "*") != 0) {
        return;
    }
    handle = FindFirstFileA(pattern, &entry);
    if (handle == INVALID_HANDLE_VALUE) {
        return;
    }
    do {
        char child[FORGE_PATH_MAX];
        int is_artifact =
            forge_util_has_suffix(entry.cFileName, ".o") ||
            forge_util_has_suffix(entry.cFileName, ".d") ||
            forge_util_has_suffix(entry.cFileName, ".cmdhash") ||
            forge_util_has_suffix(entry.cFileName, ".obj");

        if (!is_artifact ||
            forge_paths_join(child, sizeof(child), directory,
                             entry.cFileName) != 0) {
            continue;
        }
        if (!object_artifact_is_used(child, used_paths, used_count) &&
            DeleteFileA(child) != 0) {
            ++removed;
        }
    } while (FindNextFileA(handle, &entry) != 0);
    (void)FindClose(handle);
#else
    stream = opendir(directory);
    if (stream == NULL) {
        return;
    }
    while ((entry = readdir(stream)) != NULL) {
        char child[FORGE_PATH_MAX];
        int is_artifact =
            forge_util_has_suffix(entry->d_name, ".o") ||
            forge_util_has_suffix(entry->d_name, ".d") ||
            forge_util_has_suffix(entry->d_name, ".cmdhash");

        if (!is_artifact ||
            forge_paths_join(child, sizeof(child), directory,
                             entry->d_name) != 0) {
            continue;
        }
        if (!object_artifact_is_used(child, used_paths, used_count) &&
            unlink(child) == 0) {
            ++removed;
        }
    }
    (void)closedir(stream);
#endif
    if (removed != 0U) {
        forge_logger_detail(active_logger, "compile",
                            "removed %zu orphaned object file(s)", removed);
    }
}

typedef struct ForgeCompileContext {
    const ForgeCompiler *compiler;
    const ForgeBuildProfile *profile;
    const ForgeSourceList *sources;
    const ForgeStringList *extra_include_dirs;
    const char *project_version;
    char (*object_paths)[FORGE_PATH_MAX];
    const char **object_references;
    /* Per-source compile commands prepared before the pool starts (freshness
     * needs the command hash); owned by the pipeline, read-only for workers. */
    ForgeArgv *commands;
    char **command_displays;
    unsigned int *command_hashes;
    const char *project_root;
    const char *target_os;
    const char *target_arch;
    size_t count;
    size_t next;
    /* Counters and failure state shared by workers; every access holds
     * job_mutex (has_cpp_source is derived after the pool joins instead,
     * so it needs no locking). */
    size_t compiled;
    size_t skipped;
    int failed;
    char error[FORGE_COMMAND_MAX];
    ForgeMutex job_mutex;
    ForgeMutex log_mutex;
} ForgeCompileContext;

static void compile_message(ForgeCompileContext *context,
                            void (*emit)(ForgeLogger *, const char *,
                                         const char *, ...),
                            const char *stage, const char *format,
                            va_list arguments)
{
    char message[FORGE_COMMAND_MAX];

    (void)vsnprintf(message, sizeof(message), format, arguments);
    forge_mutex_lock(&context->log_mutex);
    emit(active_logger, stage, "%s", message);
    forge_mutex_unlock(&context->log_mutex);
}

static void compile_detail(ForgeCompileContext *context, const char *stage,
                           const char *format, ...)
{
    va_list arguments;

    va_start(arguments, format);
    compile_message(context, forge_logger_detail, stage, format, arguments);
    va_end(arguments);
}

static void compile_command_line(ForgeCompileContext *context, const char *stage,
                                 const char *format, ...)
{
    va_list arguments;

    va_start(arguments, format);
    compile_message(context, forge_logger_command, stage, format, arguments);
    va_end(arguments);
}

static void compile_fail(ForgeCompileContext *context, const char *message)
{
    forge_mutex_lock(&context->job_mutex);
    if (!context->failed && message != NULL && message[0] != '\0') {
        (void)snprintf(context->error, sizeof(context->error), "%s", message);
    }
    context->failed = 1;
    forge_mutex_unlock(&context->job_mutex);
}

static void compile_count(ForgeCompileContext *context, int was_skipped)
{
    forge_mutex_lock(&context->job_mutex);
    if (was_skipped) {
        ++context->skipped;
    } else {
        ++context->compiled;
    }
    forge_mutex_unlock(&context->job_mutex);
}

static void compile_source_job(ForgeCompileContext *context, size_t source_index)
{
    const char *source_path = context->sources->items[source_index].path;
    const char *object_path = context->object_paths[source_index];
    const ForgeArgv *command = &context->commands[source_index];
    const char *display = context->command_displays[source_index];
    unsigned int command_hash = context->command_hashes[source_index];
    char error[FORGE_COMMAND_MAX] = {0};
    int exit_code;
    int track_headers;

    track_headers = context->compiler->kind != FORGE_COMPILER_MSVC &&
                    context->sources->items[source_index].language != FORGE_SOURCE_ASM;
    if (forge_fingerprint_object_fresh(object_path, source_path, track_headers,
                                       command_hash)) {
        compile_detail(context, "compile", "up-to-date: %s", source_path);
        context->object_references[source_index] = object_path;
        compile_count(context, 1);
        return;
    }
    compile_detail(context, "compile", "source: %s", source_path);
    if (display != NULL) {
        compile_command_line(context, "compile", "command: %s", display);
    }
    if (forge_process_run(command->items, log_capture_path(), 1,
                          &exit_code, error, sizeof(error)) != 0) {
        compile_fail(context, error);
        return;
    }
    if (exit_code != 0) {
        char message[FORGE_COMMAND_MAX];
        (void)snprintf(message, sizeof(message), "compiler failed for %s", source_path);
        compile_fail(context, message);
        return;
    }
    /* Record what this object was built from so the next run can trust it. */
    forge_fingerprint_record_command(object_path, command_hash);
    context->object_references[source_index] = object_path;
    compile_count(context, 0);
}

static void compile_worker(void *argument)
{
    ForgeCompileContext *context = (ForgeCompileContext *)argument;

    for (;;) {
        size_t index;

        forge_mutex_lock(&context->job_mutex);
        if (context->failed) {
            forge_mutex_unlock(&context->job_mutex);
            return;
        }
        if (context->next >= context->count) {
            forge_mutex_unlock(&context->job_mutex);
            return;
        }
        index = context->next++;
        forge_mutex_unlock(&context->job_mutex);

        compile_source_job(context, index);
    }
}

/* Returns 0 on success, -1 when the compile phase failed. */
static int run_compile_pool(ForgeCompileContext *context, int max_jobs)
{
    ForgeThread *workers;
    int worker_count;
    int workers_started = 0;
    int index;

    if (context->count == 0U) {
        return 0;
    }
    worker_count = max_jobs <= 0 ? forge_thread_processor_count() : max_jobs;
    if (worker_count > (int)context->count) {
        worker_count = (int)context->count;
    }
    if (worker_count < 1) {
        worker_count = 1;
    }
    workers = calloc((size_t)worker_count, sizeof(*workers));
    if (workers == NULL) {
        print_error("out of memory while preparing build workers");
        return -1;
    }
    for (index = 0; index < worker_count; ++index) {
        if (forge_thread_spawn(&workers[index], compile_worker, context) != 0) {
            break;
        }
        ++workers_started;
    }
    for (index = 0; index < workers_started; ++index) {
        (void)forge_thread_join(&workers[index]);
    }
    free(workers);
    if (workers_started < worker_count) {
        print_error("could not start all build workers");
        return -1;
    }
    return context->failed ? -1 : 0;
}

/*
 * Emits the cargo-style end-of-build milestone. The tag mirrors cargo's
 * "[optimized]": a release profile counts as optimized when it leaves the
 * optimizer at its default or sets level 2+ itself (including through a raw
 * "-O2" style flag); everything else reports as dev.
 */
static int profile_is_optimized(const ForgeBuildProfile *profile, int release)
{
    size_t index;

    if (!release) {
        return 0;
    }
    if (profile == NULL) {
        return 0;
    }
    if (profile->opt_level < 0 || profile->opt_level >= 2) {
        return 1;
    }
    for (index = 0U; index < profile->cflags.count; ++index) {
        const char *flag = profile->cflags.items[index];

        if (strncmp(flag, "-O", 2U) == 0 &&
            strcmp(flag, "-O0") != 0 && strcmp(flag, "-O1") != 0) {
            return 1;
        }
    }
    return 0;
}

static void report_finished(const ForgeManifest *manifest, int release,
                            double started)
{
    forge_log_status("Finished", "%s %s target(s) in %.2fs",
                     release ? "release" : "debug",
                     profile_is_optimized(release ? &manifest->release_profile
                                                  : &manifest->debug_profile,
                                          release)
                         ? "[optimized]"
                         : "[dev]",
                     forge_log_monotonic_seconds() - started);
}

static int directory_exists(const char *path)
{
    struct stat details;

    return stat(path, &details) == 0 && S_ISDIR(details.st_mode);
}

/*
 * Collects the include directories of the [dependencies] `manifest` declares,
 * resolved against the caller's already-resolved graph. The graph is stored
 * post-order (a dependency always precedes its consumers), so by the time a
 * consumer is handed here every node its manifest names exists. This is what
 * lets dependency sub-builds see their own dependencies' headers without
 * re-running resolution themselves.
 */
static int collect_dep_include_dirs(const ForgeDepGraph *graph,
                                    const ForgeManifest *manifest,
                                    ForgeStringList *output,
                                    char *error, size_t error_size)
{
    size_t index;

    memset(output, 0, sizeof(*output));
    for (index = 0U; index < manifest->dependencies.count; ++index) {
        const char *name = manifest->dependencies.items[index].name;
        const ForgeDepNode *node = NULL;
        char candidate[FORGE_PATH_MAX];
        const char *chosen;
        size_t node_index;

        for (node_index = 0U; node_index < graph->count; ++node_index) {
            if (strcmp(graph->nodes[node_index].name, name) == 0) {
                node = &graph->nodes[node_index];
                break;
            }
        }
        if (node == NULL) {
            /* Unresolvable names fail loudly during resolution itself. */
            continue;
        }
        if (output->count == FORGE_MANIFEST_MAX_ITEMS) {
            forge_util_set_error(error, error_size, "too many include directories");
            return -1;
        }
        if (forge_paths_join(candidate, sizeof(candidate), node->root,
                             "include") != 0) {
            forge_util_set_error(error, error_size, "include path is too long");
            return -1;
        }
        chosen = directory_exists(candidate) ? candidate : node->root;
        if (strlen(chosen) >= sizeof(output->items[output->count])) {
            forge_util_set_error(error, error_size,
                      "include path '%s' is too long", chosen);
            return -1;
        }
        memcpy(output->items[output->count], chosen, strlen(chosen) + 1U);
        ++output->count;
    }
    return 0;
}

/*
 * The shared build pipeline behind every compile/link/run style command.
 * `sources_override` (a shallow list, never owned) replaces manifest source
 * discovery so the test runner can build one binary per test file, and
 * `output_name_override` replaces the sanitized project name as the
 * executable base name. `forced_extra_includes` (never owned) supplies the
 * -I directories for a sub-build whose [dependencies] were resolved by the
 * caller's graph instead of being resolvable from here. When
 * `resolve_dependencies` is set, [dependencies] are resolved and built first
 * and their headers/objects feed this build; dependency builds recurse with
 * the flag cleared (and without a manifest path, since dep objects carry
 * their own freshness through mtimes).
 */
static int build_binary_inner(const char *project_root, const ForgeManifest *manifest,
                              const char *manifest_path_for_freshness,
                              ForgeBuildMode mode, const ForgeBuildOptions *options,
                              const ForgeSourceList *sources_override,
                              const ForgeStringList *forced_extra_includes,
                              const char *output_name_override,
                              const char *const *program_arguments,
                              size_t program_argument_count,
                              char *built_executable, size_t built_executable_size,
                              int *child_exit_code, int resolve_dependencies)
{
    ForgeHostInfo host;
    ForgeCompiler compiler;
    ForgeSourceList sources = {0};
    int owns_sources = 0;
    /* Heap, not stack: ForgeDepGraph is ~640KB and this frame nests the
     * resolver (another manifest + materialization frames). */
    ForgeDepGraph *dep_graph = NULL;
    ForgeStringList dep_includes = {0};
    ForgePathList dep_link_inputs = {0};
    int have_deps = 0;
    size_t node_index;
    const ForgeBuildProfile *profile;
    const char *target_os;
    const char *target_arch;
    ForgeCompileContext context = {0};
    ForgeArgv argv = {0};
    char error[FORGE_COMMAND_MAX] = {0};
    char target_directory[FORGE_PATH_MAX];
    char profile_directory[FORGE_PATH_MAX];
    char object_directory[FORGE_PATH_MAX];
    char executable_name[FORGE_MANIFEST_VALUE_MAX];
    char executable_path[FORGE_PATH_MAX];
    char display[FORGE_COMMAND_MAX];
    char stamp_path[FORGE_PATH_MAX];
    char (*object_paths)[FORGE_PATH_MAX] = NULL;
    char (*used_object_names)[FORGE_PATH_MAX] = NULL;
    const char **object_references = NULL;
    ForgeBuildOptions fallback_options;
    size_t source_index;
    size_t argument_index;
    size_t used_count = 0U;
    int has_cpp_source = 0;
    int used_response_file = 0;
    int exit_code;
    int result = -1;
    /* Set by the command-prep pass; a fully fresh object set keeps the
     * Compiling milestone off (cargo prints only `Finished` then). */
    int all_objects_fresh = 1;
    /* Milestone timing: the Finished line reports total wall time. */
    double started = forge_log_monotonic_seconds();
    int release;
    int max_jobs;

    dep_graph = calloc(1U, sizeof(*dep_graph));
    if (dep_graph == NULL) {
        (void)snprintf(error, sizeof(error), "out of memory");
        result = -1;
        goto cleanup;
    }

    if (options == NULL) {
        /* Defensive default so internal callers never need to synthesize one */
        fallback_options = forge_build_default_options();
        options = &fallback_options;
    }
    release = options->release;
    max_jobs = options->max_jobs;

    if (child_exit_code != NULL) {
        *child_exit_code = 0;
    }
    if (forge_detect_host(&host, error, sizeof(error)) != 0) {
        print_error("%s", error);
        goto cleanup;
    }
    if (select_host_target(manifest, &host, &target_os, &target_arch) != 0 ||
        forge_compiler_select(&host, manifest->compiler_override, &compiler,
                              error, sizeof(error)) != 0) {
        if (error[0] != '\0') {
            print_error("%s", error);
        }
        goto cleanup;
    }
    if (sources_override != NULL) {
        sources = *sources_override;
        owns_sources = 0;
    } else {
        if (forge_sources_collect(project_root, manifest, &sources, error,
                                  sizeof(error)) != 0) {
            if (error[0] != '\0') {
                print_error("%s", error);
            }
            goto cleanup;
        }
        owns_sources = 1;
    }

    /* Resolve [dependencies] transitively and build every node before this
     * project's own sources compile: their headers feed the compile lines
     * and their objects/libraries the link line. */
    if (resolve_dependencies && manifest->dependencies.count > 0U) {
        if (forge_deps_resolve(project_root, manifest, 0, NULL,
                               options->offline, options->locked,
                               dep_graph, active_logger,
                               error, sizeof(error)) != 0) {
            print_error("%s", error);
            goto cleanup;
        }
        have_deps = 1;
        if (forge_deps_include_dirs(dep_graph, &dep_includes, error,
                                    sizeof(error)) != 0) {
            print_error("%s", error);
            goto cleanup;
        }
        if (mode != FORGE_BUILD_MODE_COMPILE_ONLY) {
            for (node_index = 0U; node_index < dep_graph->count; ++node_index) {
                ForgeDepNode *node = &dep_graph->nodes[node_index];
                char suffix[FORGE_MANIFEST_VALUE_MAX + 8U];

                version_suffix(node->manifest, suffix, sizeof(suffix));
                forge_log_status("Compiling", "%s%s", node->name, suffix);
                if (node->is_native) {
                    /*
                     * The sub-build cannot resolve its own [dependencies]
                     * (recursion clears the flag), so hand it the include
                     * directories its dependencies resolved to here.
                     */
                    ForgeStringList node_includes;
                    const ForgeStringList *forced_includes = NULL;

                    if (node->manifest->dependencies.count != 0U) {
                        if (collect_dep_include_dirs(dep_graph, node->manifest,
                                                     &node_includes, error,
                                                     sizeof(error)) != 0) {
                            print_error("%s", error);
                            goto cleanup;
                        }
                        forced_includes = &node_includes;
                    }
                    if (build_binary_inner(node->root, node->manifest,
                                           NULL,
                                           FORGE_BUILD_MODE_DEP_OBJECTS, options,
                                           NULL, forced_includes, NULL, NULL,
                                           0U, node->link_artifact,
                                           sizeof(node->link_artifact),
                                           NULL, 0) != 0) {
                        print_error("dependency '%s' failed to build", node->name);
                        goto cleanup;
                    }
                } else if (forge_deps_build_foreign(node, &compiler, release, max_jobs,
                                                    active_logger, node->link_artifact,
                                                    sizeof(node->link_artifact),
                                                    error, sizeof(error)) != 0) {
                    print_error("%s", error);
                    /* The cmake/make output landed in the invocation log;
                     * surface its tail like every other failed stage. */
                    print_log_tail(24U);
                    goto cleanup;
                }
            }
        }
    }

    profile = release ? &manifest->release_profile : &manifest->debug_profile;
    if (forge_paths_resolve(project_root, "target", target_directory,
                             sizeof(target_directory)) != 0 ||
        forge_paths_join(profile_directory, sizeof(profile_directory), target_directory,
                         release ? "release" : "debug") != 0 ||
        forge_paths_join(object_directory, sizeof(object_directory), profile_directory,
                         "obj") != 0) {
        print_error("output path is too long");
        goto cleanup;
    }
    if (forge_paths_ensure_directory(object_directory, error, sizeof(error)) != 0) {
        if (error[0] != '\0') {
            print_error("%s", error);
        }
        goto cleanup;
    }
    if (output_name_override != NULL) {
        forge_paths_safe_output_name(output_name_override, executable_name,
                                     sizeof(executable_name));
    } else {
        forge_paths_safe_output_name(manifest->project_name, executable_name,
                                     sizeof(executable_name));
    }
#if FORGE_PLATFORM_WINDOWS
    if (snprintf(executable_path, sizeof(executable_path), "%s/%s.exe",
                 profile_directory, executable_name) < 0 ||
        strlen(profile_directory) + strlen(executable_name) + 5U >= sizeof(executable_path)) {
#else
    if (forge_paths_join(executable_path, sizeof(executable_path), profile_directory,
                         executable_name) != 0) {
#endif
        print_error("executable output path is too long");
        goto cleanup;
    }
    object_paths = calloc(sources.count, sizeof(*object_paths));
    used_object_names = calloc(sources.count, sizeof(*used_object_names));
    object_references = calloc(sources.count, sizeof(*object_references));
    if (object_paths == NULL || used_object_names == NULL || object_references == NULL) {
        print_error("out of memory while preparing build outputs");
        goto cleanup;
    }

    forge_logger_detail(active_logger, "dispatch", "host=%s version=%s target=%s/%s",
                        host.os_name, host.version, target_os, target_arch);
    forge_logger_detail(active_logger, "dispatch", "%s (%s)", compiler.selection_note,
                        forge_compiler_kind_name(compiler.kind));
    if (compiler.kind == FORGE_COMPILER_MSVC) {
        forge_logger_detail(active_logger, "dispatch",
                            "MSVC writes no dependency files; header edits will not trigger recompiles");
    }

    for (source_index = 0U; source_index < sources.count; ++source_index) {
        if (forge_fingerprint_object_name(object_directory,
                                      sources.items[source_index].path,
                                      used_object_names, used_count,
                                      object_paths[source_index],
                                      sizeof(object_paths[source_index])) != 0) {
            print_error("object path is too long or object-name collisions were not resolvable");
            goto cleanup;
        }
        (void)snprintf(used_object_names[used_count], sizeof(used_object_names[used_count]),
                       "%s", object_paths[source_index]);
        ++used_count;
    }

    context.compiler = &compiler;
    context.profile = profile;
    context.sources = &sources;
    context.extra_include_dirs = forced_extra_includes != NULL
                                     ? forced_extra_includes
                                     : (have_deps ? &dep_includes : NULL);
    context.project_version = manifest->project_version;
    context.object_paths = object_paths;
    context.object_references = object_references;
    context.project_root = project_root;
    context.target_os = target_os;
    context.target_arch = target_arch;
    context.count = sources.count;
    context.next = 0U;

    /*
     * Prepare every translation unit's command line up front: the freshness
     * check needs the command hash, and knowing whether anything will
     * compile before the milestone prints is what keeps a fully up-to-date
     * rerun quiet (cargo parity — only `Finished` on a no-op build).
     */
    if (context.count != 0U) {
        context.commands = calloc(context.count, sizeof(*context.commands));
        context.command_displays = calloc(context.count,
                                          sizeof(*context.command_displays));
        context.command_hashes = calloc(context.count, sizeof(*context.command_hashes));
        if (context.commands == NULL || context.command_displays == NULL ||
            context.command_hashes == NULL) {
            print_error("out of memory while preparing compile commands");
            goto cleanup;
        }
        for (source_index = 0U; source_index < context.count; ++source_index) {
            ForgeArgv *command = &context.commands[source_index];
            char display[FORGE_COMMAND_MAX];
            char argv_error[FORGE_COMMAND_MAX] = {0};
            int track_headers =
                compiler.kind != FORGE_COMPILER_MSVC &&
                sources.items[source_index].language != FORGE_SOURCE_ASM;

            if (forge_compiler_make_compile_argv(&compiler,
                                                 sources.items[source_index].language,
                                                 sources.items[source_index].path,
                                                 object_paths[source_index],
                                                 project_root, target_os, target_arch,
                                                 profile, context.extra_include_dirs,
                                                 manifest->project_version,
                                                 command, argv_error,
                                                 sizeof(argv_error)) != 0) {
                print_error("%s", argv_error);
                goto cleanup;
            }
            (void)forge_argv_finalize(command);
            if (forge_argv_join(display, sizeof(display), command) == 0) {
                context.command_displays[source_index] = path_list_copy(display);
                context.command_hashes[source_index] =
                    forge_fingerprint_hash_text(display);
                all_objects_fresh &= forge_fingerprint_object_fresh(
                    object_paths[source_index], sources.items[source_index].path,
                    track_headers, context.command_hashes[source_index]);
            } else {
                /* An unrepresentable command line cannot match a stamp; the
                 * object simply recompiles every run. */
                all_objects_fresh = 0;
            }
        }
    }

    if (!all_objects_fresh && mode != FORGE_BUILD_MODE_DEP_OBJECTS) {
        /* Dependency sub-builds stay silent: the consumer's node loop
         * already announced them. */
        char suffix[FORGE_MANIFEST_VALUE_MAX + 8U];

        version_suffix(manifest, suffix, sizeof(suffix));
        forge_log_status(mode == FORGE_BUILD_MODE_COMPILE_ONLY ? "Checking" : "Compiling",
                         "%s%s", output_name_override != NULL
                                     ? output_name_override
                                     : manifest->project_name,
                         suffix);
    }
    forge_logger_detail(active_logger, "compile",
                        "----- compile %zu source file(s) with %d job(s) -----",
                        sources.count,
                        max_jobs <= 0 ? forge_thread_processor_count() : max_jobs);
    if (forge_mutex_init(&context.job_mutex) != 0 ||
        forge_mutex_init(&context.log_mutex) != 0) {
        forge_mutex_destroy(&context.job_mutex);
        forge_mutex_destroy(&context.log_mutex);
        print_error("out of memory while initializing build locks");
        print_log_tail(24U);
        goto cleanup;
    }
    if (run_compile_pool(&context, max_jobs) != 0) {
        forge_mutex_destroy(&context.job_mutex);
        forge_mutex_destroy(&context.log_mutex);
        print_error("%s", context.error[0] != '\0' ? context.error : "compile phase failed");
        print_log_tail(24U);
        goto cleanup;
    }
    forge_mutex_destroy(&context.job_mutex);
    forge_mutex_destroy(&context.log_mutex);

    /* Derived post-pool rather than inside workers: writing a plain flag from
     * several threads would be an unsynchronized data race. */
    for (source_index = 0U; source_index < sources.count; ++source_index) {
        if (sources.items[source_index].language == FORGE_SOURCE_CPP) {
            has_cpp_source = 1;
            break;
        }
    }
    /*
     * Dependency objects join this project's link line too, so a native
     * dependency that compiles C++ must flip the link driver as well — a
     * plain gcc/clang link would drop the C++ runtime and fail on std::
     * symbols. Foreign (CMake/Make) artifacts may need it too, but their
     * languages are unknowable here; raw cflags remain the escape hatch.
     */
    if (!has_cpp_source && have_deps) {
        for (node_index = 0U; node_index < dep_graph->count; ++node_index) {
            const ForgeDepNode *node = &dep_graph->nodes[node_index];

            if (node->manifest != NULL &&
                node->manifest->cpp_source_dirs.count > 0U) {
                has_cpp_source = 1;
                break;
            }
        }
    }
    forge_logger_detail(active_logger, "compile",
                        "compiled %zu, up-to-date %zu", context.compiled, context.skipped);

    /*
     * Sources that were renamed or deleted leave their .o/.d behind forever
     * otherwise; sweep them now that the live object set is known. Best
     * effort — a file held open by an editor or AV scanner is skipped.
     */
    remove_orphan_objects(object_directory, object_paths, sources.count);

    if (mode == FORGE_BUILD_MODE_COMPILE_ONLY) {
        forge_logger_detail(active_logger, "check",
                            "check: all %zu translation unit(s) compiled", sources.count);
        report_finished(manifest, release, started);
        result = 0;
        goto cleanup;
    }

    /* Dependency builds record their object paths so the consumer can link
     * them directly without an intermediate archive. */
    if (mode == FORGE_BUILD_MODE_DEP_OBJECTS) {
        char list_path[FORGE_PATH_MAX];

        if (forge_paths_join(list_path, sizeof(list_path), profile_directory,
                             "objects.txt") != 0) {
            print_error("object list path is too long");
            goto cleanup;
        }
        {
            ForgeObjectListBody body;
            char list_error[256];

            body.references = object_references;
            body.count = sources.count;
            if (forge_util_replace_file(list_path, write_object_list_body,
                                        &body, list_error,
                                        sizeof(list_error)) != 0) {
                print_error("could not write '%s': %s", list_path, list_error);
                goto cleanup;
            }
        }
        if (built_executable != NULL && built_executable_size != 0U) {
            (void)snprintf(built_executable, built_executable_size, "%s", list_path);
        }
        result = 0;
        goto cleanup;
    }

    /* Dependency objects/libraries resolve the project's symbols, so they
     * are collected in reverse build order (dependents first). */
    if (have_deps) {
        for (node_index = dep_graph->count; node_index-- > 0U;) {
            const ForgeDepNode *node = &dep_graph->nodes[node_index];

            if (node->is_native) {
                if (read_path_list(node->link_artifact, &dep_link_inputs) != 0) {
                    print_error("could not read '%s'", node->link_artifact);
                    goto cleanup;
                }
            } else if (node->link_artifact[0] != '\0') {
                if (path_list_push(&dep_link_inputs, node->link_artifact) != 0) {
                    print_error("out of memory while collecting dependency inputs");
                    goto cleanup;
                }
            }
        }
    }

    {
        ForgeTextBuilder expected_stamp = {0};
        struct stat executable_stat;
        int fresh;

        if ((size_t)snprintf(stamp_path, sizeof(stamp_path), "%s/%s.linkstamp",
                             profile_directory, executable_name) >= sizeof(stamp_path)) {
            print_error("link stamp path is too long");
            goto cleanup;
        }
        if (build_link_expected(&expected_stamp, manifest_path_for_freshness,
                                object_references, sources.count,
                                (const char *const *)dep_link_inputs.items,
                                dep_link_inputs.count) != 0) {
            print_error("out of memory while recording link inputs");
            text_builder_free(&expected_stamp);
            goto cleanup;
        }
        fresh = link_is_fresh(executable_path, stamp_path, &expected_stamp,
                              &executable_stat);
        if (fresh) {
            text_builder_free(&expected_stamp);
            forge_logger_detail(active_logger, "link", "up-to-date: %s", executable_path);
            if (built_executable != NULL && built_executable_size != 0U) {
                if (snprintf(built_executable, built_executable_size, "%s",
                             executable_path) < 0 ||
                    strlen(executable_path) >= built_executable_size) {
                    print_error("built executable path is too long");
                    goto cleanup;
                }
            }
            report_finished(manifest, release, started);
            if (mode != FORGE_BUILD_MODE_RUN) {
                result = 0;
                goto cleanup;
            }
            goto run_phase;
        }

        argv = (ForgeArgv){0};
        if (forge_compiler_make_link_argv(&compiler, has_cpp_source, object_references,
                                          sources.count,
                                          (const char *const *)dep_link_inputs.items,
                                          dep_link_inputs.count, executable_path,
                                           profile_directory, profile, &argv,
                                           &used_response_file, error,
                                           sizeof(error)) != 0) {
            print_error("%s", error);
            text_builder_free(&expected_stamp);
            goto cleanup;
        }
        forge_logger_detail(active_logger, "link", "----- link %s -----", executable_path);
        (void)forge_argv_finalize(&argv);
        if (forge_argv_join(display, sizeof(display), &argv) == 0) {
            forge_logger_command(active_logger, "link", "command: %s", display);
        }
        if (used_response_file) {
            forge_logger_detail(active_logger, "link",
                                "long link line: objects and flags spilled to %s/link.rsp",
                                profile_directory);
        }
        exit_code = 0;
        if (forge_process_run(argv.items, log_capture_path(), 1,
                              &exit_code, error, sizeof(error)) != 0) {
            print_error("%s", error);
            forge_argv_free(&argv);
            goto cleanup;
        }
        forge_argv_free(&argv);
        if (exit_code != 0) {
            print_error("linker failed");
            print_log_tail(24U);
            goto cleanup;
        }
        write_link_stamp(stamp_path, &expected_stamp, executable_name);
        text_builder_free(&expected_stamp);
    }
    if (built_executable != NULL && built_executable_size != 0U) {
        if (snprintf(built_executable, built_executable_size, "%s", executable_path) < 0 ||
            strlen(executable_path) >= built_executable_size) {
            print_error("built executable path is too long");
            goto cleanup;
        }
    }
    report_finished(manifest, release, started);
    if (mode != FORGE_BUILD_MODE_RUN) {
        result = 0;
        goto cleanup;
    }

run_phase:
    forge_log_status("Running", "%s", executable_path);
    forge_logger_detail(active_logger, "run", "----- run %s -----", executable_path);
    argv = (ForgeArgv){0};
    if (forge_argv_append(&argv, executable_path) != 0) {
        print_error("out of memory while preparing run command");
        goto cleanup;
    }
    for (argument_index = 0U; argument_index < program_argument_count; ++argument_index) {
        if (forge_argv_append(&argv, program_arguments[argument_index]) != 0) {
            print_error("out of memory while preparing run command");
            goto cleanup;
        }
    }
    (void)forge_argv_finalize(&argv);
    if (forge_argv_join(display, sizeof(display), &argv) == 0) {
        forge_logger_command(active_logger, "run", "program: %s", display);
    }
    exit_code = 0;
    if (forge_process_run(argv.items, NULL, 0, &exit_code, error, sizeof(error)) != 0) {
        forge_argv_free(&argv);
        print_error("%s", error);
        goto cleanup;
    }
    forge_argv_free(&argv);
    if (child_exit_code != NULL) {
        *child_exit_code = exit_code;
    }
    /* A failing program is not a failing build pipeline; callers decide what
     * the child's code means (forge run propagates it, forge test records it).
     * The nonzero exit is surfaced on the terminal even at -q, where the
     * program's own output would otherwise be the only clue. */
    if (exit_code != 0) {
        forge_logger_error(active_logger, "run", "exit: %d", exit_code);
    }
    result = 0;

cleanup:
    forge_argv_free(&argv);
    if (context.commands != NULL) {
        for (source_index = 0U; source_index < context.count; ++source_index) {
            forge_argv_free(&context.commands[source_index]);
        }
    }
    free(context.commands);
    if (context.command_displays != NULL) {
        for (source_index = 0U; source_index < context.count; ++source_index) {
            free(context.command_displays[source_index]);
        }
    }
    free(context.command_displays);
    free(context.command_hashes);
    free(object_references);
    free(used_object_names);
    free(object_paths);
    if (owns_sources) {
        forge_sources_free(&sources);
    }
    path_list_free(&dep_link_inputs);
    forge_deps_free_graph(dep_graph);
    free(dep_graph);
    return result;
}

ForgeBuildOptions forge_build_default_options(void)
{
    ForgeBuildOptions options;

    options.release = 0;
    options.max_jobs = 0;
    options.offline = 0;
    options.locked = 0;
    return options;
}

int forge_build_project(const char *project_root, const ForgeManifest *manifest,
                        const char *manifest_path,
                        ForgeBuildMode mode, const ForgeBuildOptions *options,
                        const char *const *program_arguments,
                        size_t program_argument_count,
                        char *built_executable, size_t built_executable_size,
                        int *child_exit_code)
{
    return build_binary_inner(project_root, manifest, manifest_path, mode, options,
                              NULL, NULL, NULL, program_arguments,
                              program_argument_count, built_executable,
                              built_executable_size, child_exit_code, 1);
}

/* Extracts the sanitized base name of `path` without its extension so each
 * test file becomes its own executable name. */
static void test_binary_name(const char *path, char *output, size_t output_size)
{
    const char *slash = strrchr(path, '/');
    const char *backslash = strrchr(path, '\\');
    const char *base = path;
    const char *dot;
    char stem[FORGE_MANIFEST_VALUE_MAX];
    size_t length;

    if (slash != NULL && slash + 1 > base) {
        base = slash + 1;
    }
    if (backslash != NULL && backslash + 1 > base) {
        base = backslash + 1;
    }
    dot = strrchr(base, '.');
    length = dot != NULL && dot > base ? (size_t)(dot - base) : strlen(base);
    if (length >= sizeof(stem)) {
        length = sizeof(stem) - 1U;
    }
    memcpy(stem, base, length);
    stem[length] = '\0';
    forge_paths_safe_output_name(stem, output, output_size);
}

/*
 * Builds and runs every standalone test source in <root>/tests. Each test
 * file must be self-contained (own main); binaries land next to the normal
 * build outputs and run in the requested profile. C, C++, and assembly
 * sources are all eligible. When `test_filter` is non-NULL only the test
 * whose binary name equals it runs, and a filter matching no test is an
 * error. Returns 0 when every executed test passed, 1 when any failed,
 * -1 when the harness itself broke (including a filter that matched nothing).
 */
int forge_build_tests(const char *project_root, const ForgeManifest *manifest,
                      const char *manifest_path,
                      const ForgeBuildOptions *options, const char *test_filter)
{
    ForgeManifest test_manifest = *manifest;
    ForgeSourceList tests = {0};
    char error[FORGE_COMMAND_MAX] = {0};
    char tests_directory[FORGE_PATH_MAX];
    struct stat details;
    size_t index;
    size_t passed = 0U;
    size_t failed = 0U;
    size_t run_count = 0U;

    if (forge_paths_join(tests_directory, sizeof(tests_directory), project_root,
                         "tests") != 0) {
        print_error("tests directory path is too long");
        return -1;
    }
    /* No tests/ directory is a normal state, not an error. */
    if (stat(tests_directory, &details) != 0 || !S_ISDIR(details.st_mode)) {
        forge_logger_detail(active_logger, "test",
                            "no tests found in tests/ (create tests/*.c with their own main)");
        return 0;
    }
    /* All three language buckets point at tests/ so each collected file is
     * tagged with the language its extension implies. */
    test_manifest.c_source_dirs.count = 1U;
    (void)snprintf(test_manifest.c_source_dirs.items[0],
                   sizeof(test_manifest.c_source_dirs.items[0]), "%s", "tests");
    test_manifest.cpp_source_dirs.count = 1U;
    (void)snprintf(test_manifest.cpp_source_dirs.items[0],
                   sizeof(test_manifest.cpp_source_dirs.items[0]), "%s", "tests");
    test_manifest.asm_source_dirs.count = 1U;
    (void)snprintf(test_manifest.asm_source_dirs.items[0],
                   sizeof(test_manifest.asm_source_dirs.items[0]), "%s", "tests");
    if (forge_sources_collect(project_root, &test_manifest, &tests, error,
                              sizeof(error)) != 0) {
        print_error("%s", error);
        return -1;
    }
    if (tests.count == 0U) {
        forge_logger_detail(active_logger, "test",
                            "no tests found in tests/ (create tests/*.c with their own main)");
        forge_sources_free(&tests);
        return 0;
    }
    for (index = 0U; index < tests.count; ++index) {
        ForgeSourceList single;
        char name[FORGE_MANIFEST_VALUE_MAX];
        int child_exit_code = 0;
        int status;

        test_binary_name(tests.items[index].path, name, sizeof(name));
        if (test_filter != NULL && strcmp(name, test_filter) != 0 &&
            strcmp(tests.items[index].path, test_filter) != 0) {
            continue;
        }
        ++run_count;
        single.items = &tests.items[index];
        single.count = 1U;
        single.capacity = 0U;
        forge_log_status("Running", "test %s (%s)", name, tests.items[index].path);
        forge_logger_detail(active_logger, "test", "----- test %s (%s) -----",
                            name, tests.items[index].path);
        status = build_binary_inner(project_root, manifest, manifest_path,
                                    FORGE_BUILD_MODE_RUN, options,
                                    &single, NULL, name, NULL, 0U, NULL, 0U,
                                    &child_exit_code, 1);
        if (status != 0) {
            ++failed;
            print_log_tail(24U);
            continue;
        }
        if (child_exit_code == 0) {
            ++passed;
            forge_logger_detail(active_logger, "test", "passed: %s", name);
        } else {
            ++failed;
            forge_logger_error(active_logger, "test", "failed: %s (exit %d)",
                               name, child_exit_code);
        }
    }
    forge_sources_free(&tests);
    if (test_filter != NULL && run_count == 0U) {
        /* A filter that matches nothing is a user error, not a passing
         * suite: silently exiting 0 could hide a typo'd --test name. */
        forge_logger_error(active_logger, "test", "no test matched '%s'",
                           test_filter);
        return -1;
    }
    forge_log_status("Test result", "%zu passed; %zu failed", passed, failed);
    forge_logger_detail(active_logger, "test", "test result: %zu passed; %zu failed",
                        passed, failed);
    return failed == 0U ? 0 : 1;
}

void forge_build_set_logger(ForgeLogger *logger)
{
    active_logger = logger;
}

/* Removes the project's target/ directory, anchored to its manifest. */
int forge_build_clean(const char *manifest_path)
{
    char project_root[FORGE_PATH_MAX];
    char target_path[FORGE_PATH_MAX];

    if (forge_build_project_root(manifest_path, project_root, sizeof(project_root)) != 0) {
        return 1;
    }
    if (forge_paths_resolve(project_root, "target", target_path,
                            sizeof(target_path)) != 0) {
        print_error("clean path is too long");
        return 1;
    }
    printf("forge: removing %s\n", target_path);
    if (forge_paths_remove_tree(target_path, NULL, 0U) != 0) {
        return 1;
    }
    printf("forge: clean\n");
    return 0;
}
