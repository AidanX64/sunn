#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "forge/build.h"
#include "forge/deps.h"
#include "forge/manifest.h"
#include "forge/paths.h"
#include "forge/pkg.h"
#include "forge/registry.h"
#include "forge_util.h"

/*
 * Whole-file line editor for [dependencies]. Every line keeps its original
 * terminator, so adding or removing one entry leaves the rest of the file
 * byte-for-byte identical (comments, ordering, and formatting included).
 */
typedef struct ForgeLineList {
    char **items;
    size_t count;
    size_t capacity;
} ForgeLineList;

static void line_list_free(ForgeLineList *list)
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

/* Copies `length` bytes (terminators included) as one stored line. */
static int line_list_push_n(ForgeLineList *list, const char *text, size_t length)
{
    if (list->count == list->capacity) {
        size_t new_capacity = list->capacity == 0U ? 32U : list->capacity * 2U;
        char **grown = realloc(list->items, new_capacity * sizeof(*grown));

        if (grown == NULL) {
            return -1;
        }
        list->items = grown;
        list->capacity = new_capacity;
    }
    {
        char *copy = malloc(length + 1U);

        if (copy == NULL) {
            return -1;
        }
        memcpy(copy, text, length);
        copy[length] = '\0';
        list->items[list->count] = copy;
    }
    ++list->count;
    return 0;
}

static int line_list_push(ForgeLineList *list, const char *text)
{
    return line_list_push_n(list, text, strlen(text));
}

static int line_list_clone(const ForgeLineList *source, ForgeLineList *clone)
{
    size_t index;

    memset(clone, 0, sizeof(*clone));
    for (index = 0U; index < source->count; ++index) {
        if (line_list_push(clone, source->items[index]) != 0) {
            line_list_free(clone);
            return -1;
        }
    }
    return 0;
}

/*
 * Reads every line of `path` character-wise so arbitrarily long lines
 * survive a rewrite intact instead of being split by a fixed buffer.
 */
static int read_lines(const char *path, ForgeLineList *lines,
                      char *error, size_t error_size)
{
    FILE *file = fopen(path, "rb");
    char *buffer = NULL;
    size_t length = 0U;
    size_t capacity = 0U;
    int failed = 0;

    if (file == NULL) {
        forge_util_set_error(error, error_size, "could not open manifest '%s'", path);
        return -1;
    }
    for (;;) {
        int character = fgetc(file);

        if (character == EOF && length == 0U) {
            break;
        }
        if (character != EOF && character != '\n') {
            if (length + 1U > capacity) {
                size_t new_capacity = capacity == 0U ? 128U : capacity * 2U;
                char *grown;

                if (new_capacity < length + 1U) {
                    new_capacity = length + 1U;
                }
                grown = realloc(buffer, new_capacity);
                if (grown == NULL) {
                    failed = 1;
                    break;
                }
                buffer = grown;
                capacity = new_capacity;
            }
            buffer[length++] = (char)character;
            continue;
        }
        /* A newline joins the stored line; a final unterminated line does not. */
        if (character == '\n') {
            if (length + 1U > capacity) {
                size_t new_capacity = capacity == 0U ? 128U : capacity * 2U;
                char *grown;

                if (new_capacity < length + 2U) {
                    new_capacity = length + 2U;
                }
                grown = realloc(buffer, new_capacity);
                if (grown == NULL) {
                    failed = 1;
                    break;
                }
                buffer = grown;
                capacity = new_capacity;
            }
            buffer[length++] = '\n';
        }
        if (line_list_push_n(lines, buffer == NULL ? "" : buffer, length) != 0) {
            failed = 1;
            break;
        }
        length = 0U;
        if (character == EOF) {
            break;
        }
    }
    free(buffer);
    (void)fclose(file);
    if (failed) {
        forge_util_set_error(error, error_size,
                  "out of memory while reading '%s'", path);
        return -1;
    }
    return 0;
}

static int write_manifest_body(void *user_data, FILE *file)
{
    const ForgeLineList *lines = user_data;
    size_t index;

    for (index = 0U; index < lines->count; ++index) {
        if (fputs(lines->items[index], file) < 0) {
            return -1;
        }
    }
    return 0;
}

/* Atomic via tmp+rename: a crash mid-add/remove must not truncate the
 * manifest, and concurrent forge invocations must never observe half a
 * rewrite. */
static int write_lines(const char *path, const ForgeLineList *lines,
                       char *error, size_t error_size)
{
    return forge_util_replace_file(path, write_manifest_body, (void *)lines,
                                   error, error_size);
}

/* Copies `text` with '"' and '\' escaped the way the manifest parser reads
 * them back ("\\", "\""), so Windows paths survive the round-trip. */
static char *escape_manifest_string(const char *text)
{
    size_t length = strlen(text);
    size_t index;
    size_t position = 0U;
    char *out = malloc(length * 2U + 1U);

    if (out == NULL) {
        return NULL;
    }
    for (index = 0U; index < length; ++index) {
        if (text[index] == '"' || text[index] == '\\') {
            out[position++] = '\\';
        }
        out[position++] = text[index];
    }
    out[position] = '\0';
    return out;
}

/* Same portable charset the manifest parser enforces for dependency names. */
static int dependency_name_is_portable(const char *name)
{
    size_t index;

    if (name == NULL || name[0] == '\0') {
        return 0;
    }
    for (index = 0U; name[index] != '\0'; ++index) {
        unsigned char character = (unsigned char)name[index];

        if (!((character >= 'a' && character <= 'z') ||
              (character >= 'A' && character <= 'Z') ||
              (character >= '0' && character <= '9') ||
              character == '-' || character == '_' || character == '.')) {
            return 0;
        }
    }
    return 1;
}

/* Whitespace-skipping helpers that never mutate the line they inspect. */
static const char *skip_blank(const char *text)
{
    while (*text == ' ' || *text == '\t' || *text == '\r' || *text == '\n') {
        ++text;
    }
    return text;
}

static int section_header_equals(const char *line, const char *name)
{
    const char *cursor = skip_blank(line);
    size_t name_length = strlen(name);

    if (*cursor != '[') {
        return 0;
    }
    ++cursor;
    if (strncmp(cursor, name, name_length) != 0) {
        return 0;
    }
    cursor += name_length;
    while (*cursor == ' ' || *cursor == '\t') {
        ++cursor;
    }
    return *cursor == ']';
}

static int find_dependencies_header(const ForgeLineList *lines)
{
    size_t index;

    for (index = 0U; index < lines->count; ++index) {
        if (section_header_equals(lines->items[index], "dependencies")) {
            return (int)index;
        }
    }
    return -1;
}

/* Copies the trimmed key of a `key = value` line without mutating it.
 * Returns 1 when `line` is an assignment and the key fits `key_size`. */
static int entry_key_of(const char *line, char *key, size_t key_size)
{
    const char *start = skip_blank(line);
    const char *cursor = start;
    const char *end;
    size_t length;

    while (*cursor != '\0' && *cursor != '=' && *cursor != '[') {
        ++cursor;
    }
    if (*cursor != '=') {
        return 0;
    }
    end = cursor;
    while (end > start && (end[-1] == ' ' || end[-1] == '\t' ||
                           end[-1] == '\r' || end[-1] == '\n')) {
        --end;
    }
    length = (size_t)(end - start);
    if (length == 0U || length >= key_size) {
        return 0;
    }
    memcpy(key, start, length);
    key[length] = '\0';
    return 1;
}

/* Inserts `entry_line` (a newline is appended) after the [dependencies]
 * header, or appends a fresh section at the end of the file. */
static int insert_dependency_line(ForgeLineList *lines, const char *entry_line)
{
    int header = find_dependencies_header(lines);
    size_t insert_at;
    size_t index;
    char *copy;
    size_t copy_length;

    if (header >= 0) {
        const char *header_line = lines->items[header];
        size_t header_length = strlen(header_line);

        /*
         * A file whose very last line is the header may have been stored
         * without its newline; splicing after it would glue the entry onto
         * the header ("[dependencies]x = ...") and break the manifest. Give
         * the header its terminator first.
         */
        if (header_length != 0U && header_line[header_length - 1U] != '\n') {
            char *terminated = malloc(header_length + 2U);

            if (terminated == NULL) {
                return -1;
            }
            memcpy(terminated, header_line, header_length);
            terminated[header_length] = '\n';
            terminated[header_length + 1U] = '\0';
            free(lines->items[header]);
            lines->items[header] = terminated;
        }
        insert_at = (size_t)header + 1U;
    } else {
        if (lines->count != 0U &&
            *skip_blank(lines->items[lines->count - 1U]) != '\0' &&
            line_list_push(lines, "\n") != 0) {
            return -1;
        }
        if (line_list_push(lines, "[dependencies]\n") != 0) {
            return -1;
        }
        insert_at = lines->count;
    }

    copy_length = strlen(entry_line) + 2U;
    copy = malloc(copy_length);
    if (copy == NULL) {
        return -1;
    }
    (void)snprintf(copy, copy_length, "%s\n", entry_line);

    if (lines->count == lines->capacity) {
        size_t new_capacity = lines->capacity == 0U ? 32U : lines->capacity * 2U;
        char **grown = realloc(lines->items, new_capacity * sizeof(*grown));

        if (grown == NULL) {
            free(copy);
            return -1;
        }
        lines->items = grown;
        lines->capacity = new_capacity;
    }
    for (index = lines->count; index > insert_at; --index) {
        lines->items[index] = lines->items[index - 1U];
    }
    lines->items[insert_at] = copy;
    ++lines->count;
    return 0;
}

/* Removes the entry whose key equals `name`; returns 1 when removed. */
static int remove_dependency_line(ForgeLineList *lines, const char *name){
    int header = find_dependencies_header(lines);
    size_t index;

    if (header < 0) {
        return 0;
    }
    for (index = (size_t)header + 1U; index < lines->count; ++index) {
        const char *cursor = skip_blank(lines->items[index]);
        char key[FORGE_MANIFEST_VALUE_MAX];

        if (*cursor == '\0' || *cursor == '#') {
            continue;
        }
        if (*cursor == '[') {
            break; /* the next section began: entries are over */
        }
        if (entry_key_of(lines->items[index], key, sizeof(key)) &&
            strcmp(key, name) == 0) {
            free(lines->items[index]);
            memmove(&lines->items[index], &lines->items[index + 1U],
                    (lines->count - index - 1U) * sizeof(*lines->items));
            --lines->count;
            return 1;
        }
    }
    return 0;
}

/*
 * Re-resolves [dependencies] against the manifest now on disk so Forge.lock
 * gains or loses exactly the pins the edit implies. The manifest is reloaded
 * rather than reused from the caller because the edit changed it.
 */
static int resolve_after_edit(const char *manifest_path, ForgeLogger *logger,
                              char *error, size_t error_size)
{
    /* Heap, not stack: ForgeManifest (~350KB) plus ForgeDepGraph (~640KB)
     * nest inside forge_pkg_add's own manifest, and the resolver below
     * materializes packages (more frames) before the next manifest load.
     * That ~1.4MB used to overflow the Windows stack mid-resolve. */
    ForgeManifest *manifest = calloc(1U, sizeof(*manifest));
    ForgeDepGraph *graph = calloc(1U, sizeof(*graph));
    char root[FORGE_PATH_MAX];
    int status = -1;

    if (manifest == NULL || graph == NULL) {
        free(manifest);
        free(graph);
        forge_util_set_error(error, error_size, "out of memory");
        return -1;
    }
    if (forge_manifest_load(manifest_path, manifest, error, error_size) != 0) {
        goto done;
    }
    if (forge_build_project_root(manifest_path, root, sizeof(root)) != 0) {
        goto done;
    }
    status = forge_deps_resolve(root, manifest, 0, NULL, 0, 0, graph, logger, error,
                                error_size);
    forge_deps_free_graph(graph);
done:
    free(manifest);
    free(graph);
    return status;
}

/*
 * Restores `backup` over the manifest after a failed edit and reports
 * `cause` alongside whether the revert itself succeeded. `error` may be
 * NULL (best-effort revert during an already-failing write).
 */
static void revert_manifest_edit(const char *manifest_path,
                                 const ForgeLineList *backup, const char *cause,
                                 char *error, size_t error_size)
{
    if (write_lines(manifest_path, backup, NULL, 0U) != 0) {
        forge_util_set_error(error, error_size,
                  "%s (the manifest edit could NOT be reverted; fix %s by hand)",
                  cause, manifest_path);
        return;
    }
    forge_util_set_error(error, error_size, "%s (the manifest edit was reverted)",
              cause);
}

int forge_pkg_add(const char *manifest_path, const char *name,
                  const char *git_url, const char *ref_kind,
                  const char *ref_value, const char *dep_path,
                  const char *registry_package, const char *registry_version,
                  const char *registry_min_version,
                  const char *registry_features,
                  int registry_no_default_features,
                  ForgeLogger *logger, char *error, size_t error_size)
{
    static const char *const ref_kinds[] = { "tag", "branch", "rev" };
    /* Heap, not stack: ForgeManifest is ~360KB and this frame nests the
     * resolver (graph + another manifest + materialization frames). */
    ForgeManifest *manifest = NULL;
    ForgeLineList lines = {0};
    ForgeLineList backup = {0};
    char *escaped_value = NULL;
    char *escaped_ref = NULL;
    char *escaped_second = NULL;
    char *escaped_features = NULL;
    char *entry_line = NULL;
    char resolved_version[FORGE_MANIFEST_VALUE_MAX] = {0};
    char resolved_min[FORGE_MANIFEST_VALUE_MAX] = {0};
    char joined_features[FORGE_FEATURES_JOINED_MAX] = {0};
    size_t needed;
    size_t index;
    int has_git = git_url != NULL && git_url[0] != '\0';
    int has_path = dep_path != NULL && dep_path[0] != '\0';
    int has_registry = registry_package != NULL && registry_package[0] != '\0';
    int has_ref = ref_value != NULL && ref_value[0] != '\0';
    int has_reg_version = registry_version != NULL && registry_version[0] != '\0';
    int has_reg_min = registry_min_version != NULL && registry_min_version[0] != '\0';
    int has_reg_features = registry_features != NULL && registry_features[0] != '\0';
    const char *kind = ref_kind != NULL ? ref_kind : "";

    if (manifest_path == NULL || name == NULL) {
        forge_util_set_error(error, error_size, "manifest path and dependency name are required");
        return -1;
    }
    if (has_git + has_path + has_registry != 1) {
        forge_util_set_error(error, error_size,
                  "'%s' needs exactly one source: --git URL, --path "
                  "DIRECTORY, or --registry PACKAGE", name);
        return -1;
    }
    if (!dependency_name_is_portable(name)) {
        forge_util_set_error(error, error_size,
                  "'%s' is not a valid dependency name; use letters, digits, '-', '_', '.'",
                  name);
        return -1;
    }
    if (has_git) {
        char url_reason[FORGE_COMMAND_MAX];

        /* Same transport allowlist the resolver enforces, checked here so a
         * bad URL never reaches the manifest in the first place. */
        if (forge_deps_git_url_is_supported(git_url, url_reason,
                                            sizeof(url_reason)) != 0) {
            forge_util_set_error(error, error_size, "'%s': %s", name, url_reason);
            return -1;
        }
    }
    if (has_ref && !has_git) {
        forge_util_set_error(error, error_size,
                  "'%s': tag/branch/rev only apply to git dependencies", name);
        return -1;
    }
    if (has_reg_version && !has_registry) {
        forge_util_set_error(error, error_size,
                  "'%s': --version only applies to registry dependencies", name);
        return -1;
    }
    if (has_reg_min && !has_registry) {
        forge_util_set_error(error, error_size,
                  "'%s': --min-version only applies to registry dependencies", name);
        return -1;
    }
    if (has_reg_version && has_reg_min) {
        forge_util_set_error(error, error_size,
                  "'%s': use only one of --version/--min-version", name);
        return -1;
    }
    if ((has_reg_features || registry_no_default_features) && !has_registry) {
        forge_util_set_error(error, error_size,
                  "'%s': --features/--no-default-features only apply to registry dependencies", name);
        return -1;
    }
    if (has_reg_features) {
        ForgeDependency scratch;

        memset(&scratch, 0, sizeof(scratch));
        if (forge_parse_feature_list(name, registry_features, &scratch,
                                     error, error_size) != 0) {
            return -1;
        }
        if (forge_features_join(scratch.feature_count, scratch.features,
                                joined_features,
                                sizeof(joined_features)) != 0) {
            forge_util_set_error(error, error_size,
                      "'%s': feature set is too large", name);
            return -1;
        }
        has_reg_features = joined_features[0] != '\0';
    }
    if (has_registry && !dependency_name_is_portable(registry_package)) {
        forge_util_set_error(error, error_size,
                  "'%s' is not a valid registry package name; use letters, "
                  "digits, '-', '_', '.'", registry_package);
        return -1;
    }
    if (has_ref) {
        int kind_known = 0;

        for (index = 0U; index < 3U; ++index) {
            if (strcmp(kind, ref_kinds[index]) == 0) {
                kind_known = 1;
                break;
            }
        }
        if (!kind_known) {
            forge_util_set_error(error, error_size,
                      "'%s': unknown ref kind '%s'; use tag, branch, or rev",
                      name, kind);
            return -1;
        }
    }
    /* Never edit a manifest forge cannot parse: typos elsewhere in the file
     * would be silently destroyed by the rewrite. */
    manifest = calloc(1U, sizeof(*manifest));
    if (manifest == NULL) {
        forge_util_set_error(error, error_size, "out of memory");
        return -1;
    }
    if (forge_manifest_load(manifest_path, manifest, error, error_size) != 0) {
        free(manifest);
        return -1;
    }
    for (index = 0U; index < manifest->dependencies.count; ++index) {
        if (strcmp(manifest->dependencies.items[index].name, name) == 0) {
            forge_util_set_error(error, error_size,
                      "dependency '%s' already exists in %s", name, manifest_path);
            free(manifest);
            return -1;
        }
    }
    if (has_registry) {
        /*
         * An exact pin is written verbatim; a minimum is written as
         * min-version (validated when the manifest reloads below). A bare
         * entry names no version at all and resolves to the baseline at
         * resolve time. The resolve below verifies every shape and rolls
         * back on failure.
         */
        if (has_reg_version) {
            if (snprintf(resolved_version, sizeof(resolved_version), "%s",
                         registry_version) < 0 ||
                strlen(registry_version) >= sizeof(resolved_version)) {
                forge_util_set_error(error, error_size, "registry version is too long");
                goto fail_before_write;
            }
        } else if (has_reg_min) {
            if (snprintf(resolved_min, sizeof(resolved_min), "%s",
                         registry_min_version) < 0 ||
                strlen(registry_min_version) >= sizeof(resolved_min)) {
                forge_util_set_error(error, error_size, "registry version is too long");
                goto fail_before_write;
            }
        }
    }

    escaped_value = escape_manifest_string(has_registry ? registry_package :
                                           has_git ? git_url : dep_path);
    if (has_ref) {
        escaped_ref = escape_manifest_string(ref_value);
    }
    if (has_reg_version) {
        escaped_second = escape_manifest_string(resolved_version);
    } else if (has_reg_min) {
        escaped_second = escape_manifest_string(resolved_min);
    }
    if (has_reg_features) {
        escaped_features = escape_manifest_string(joined_features);
    }
    if (escaped_value == NULL ||
        ((has_reg_version || has_reg_min) && escaped_second == NULL) ||
        (has_reg_features && escaped_features == NULL) ||
        (has_ref && escaped_ref == NULL)) {
        forge_util_set_error(error, error_size, "out of memory");
        goto fail_before_write;
    }
    /* Optional registry segments, built first so the entry line can be
     * sized exactly: an exact/minimum pin, a feature request, and the
     * defaults opt-out compose freely. */
    char version_segment[FORGE_MANIFEST_VALUE_MAX + 32U] = {0};
    char features_segment[FORGE_FEATURES_JOINED_MAX + 32U] = {0};
    char defaults_segment[32] = {0};

    if (has_reg_version) {
        (void)snprintf(version_segment, sizeof(version_segment),
                       ", version = \"%s\"", escaped_second);
    } else if (has_reg_min) {
        (void)snprintf(version_segment, sizeof(version_segment),
                       ", min-version = \"%s\"", escaped_second);
    }
    if (has_reg_features) {
        (void)snprintf(features_segment, sizeof(features_segment),
                       ", features = \"%s\"", escaped_features);
    }
    if (registry_no_default_features) {
        (void)snprintf(defaults_segment, sizeof(defaults_segment),
                       "%s", ", default-features = \"false\"");
    }
    needed = strlen(name) + strlen(escaped_value) + 48U +
             (has_ref ? strlen(kind) + strlen(escaped_ref) + 8U : 0U) +
             strlen(version_segment) + strlen(features_segment) +
             strlen(defaults_segment);
    entry_line = malloc(needed);
    if (entry_line == NULL) {
        forge_util_set_error(error, error_size, "out of memory");
        goto fail_before_write;
    }
    if (has_registry) {
        (void)snprintf(entry_line, needed, "%s = { registry = \"%s\"%s%s%s }",
                       name, escaped_value, version_segment,
                       features_segment, defaults_segment);
    } else if (has_git) {
        if (has_ref) {
            (void)snprintf(entry_line, needed, "%s = { git = \"%s\", %s = \"%s\" }",
                           name, escaped_value, kind, escaped_ref);
        } else {
            (void)snprintf(entry_line, needed, "%s = { git = \"%s\" }",
                           name, escaped_value);
        }
    } else {
        (void)snprintf(entry_line, needed, "%s = { path = \"%s\" }",
                       name, escaped_value);
    }

    if (read_lines(manifest_path, &lines, error, error_size) != 0 ||
        line_list_clone(&lines, &backup) != 0) {
        if (backup.items == NULL && lines.count != 0U) {
            forge_util_set_error(error, error_size, "out of memory");
        }
        goto fail_before_write;
    }
    if (insert_dependency_line(&lines, entry_line) != 0) {
        forge_util_set_error(error, error_size, "out of memory while editing '%s'",
                  manifest_path);
        goto fail_before_write;
    }
    if (write_lines(manifest_path, &lines, error, error_size) != 0) {
        char cause[FORGE_COMMAND_MAX];

        /*
         * The write truncates before it fills, so the file may now hold only
         * part of the edit; put the original bytes back no matter what.
         */
        (void)snprintf(cause, sizeof(cause), "%s",
                       error != NULL && error[0] != '\0' ? error
                                                         : "manifest could not be written");
        revert_manifest_edit(manifest_path, &backup, cause, error, error_size);
        goto fail_after_write;
    }
    printf("forge: added %s to %s\n", name, manifest_path);

    /* Pin the new dependency immediately; undo the edit if it cannot resolve. */
    if (resolve_after_edit(manifest_path, logger, error, error_size) != 0) {
        char cause[FORGE_COMMAND_MAX];

        (void)snprintf(cause, sizeof(cause), "%s",
                       error != NULL && error[0] != '\0' ? error
                                                         : "dependency could not be resolved");
        revert_manifest_edit(manifest_path, &backup, cause, error, error_size);
        goto fail_after_write;
    }
    printf("forge: resolved %s and updated Forge.lock\n", name);

    free(manifest);
    free(entry_line);
    free(escaped_value);
    free(escaped_ref);
    free(escaped_second);
    free(escaped_features);
    line_list_free(&lines);
    line_list_free(&backup);
    return 0;

fail_after_write:
fail_before_write:
    free(manifest);
    free(entry_line);
    free(escaped_value);
    free(escaped_ref);
    free(escaped_second);
    free(escaped_features);
    line_list_free(&lines);
    line_list_free(&backup);
    return -1;
}

int forge_pkg_remove(const char *manifest_path, const char *name,
                     ForgeLogger *logger, char *error, size_t error_size)
{
    /* Heap, not stack: ForgeManifest is ~360KB (see forge_pkg_add). */
    ForgeManifest *manifest = NULL;
    ForgeLineList lines = {0};
    char available[FORGE_COMMAND_MAX];
    size_t index;
    size_t used = 0U;
    int status = -1;

    if (manifest_path == NULL || name == NULL) {
        forge_util_set_error(error, error_size, "manifest path and dependency name are required");
        return -1;
    }
    manifest = calloc(1U, sizeof(*manifest));
    if (manifest == NULL) {
        forge_util_set_error(error, error_size, "out of memory");
        return -1;
    }
    if (forge_manifest_load(manifest_path, manifest, error, error_size) != 0) {
        goto done;
    }
    for (index = 0U; index < manifest->dependencies.count; ++index) {
        if (strcmp(manifest->dependencies.items[index].name, name) == 0) {
            break;
        }
    }
    if (index == manifest->dependencies.count) {
        for (index = 0U; index < manifest->dependencies.count; ++index) {
            int written = snprintf(available + used, sizeof(available) - used,
                                   "%s%s", index != 0U ? ", " : "",
                                   manifest->dependencies.items[index].name);

            if (written < 0 || (size_t)written >= sizeof(available) - used) {
                break;
            }
            used += (size_t)written;
        }
        forge_util_set_error(error, error_size,
                  "dependency '%s' is not declared in %s (declared: %s)",
                  name, manifest_path,
                  manifest->dependencies.count == 0U ? "none" : available);
        goto done;
    }
    if (read_lines(manifest_path, &lines, error, error_size) != 0) {
        goto done;
    }
    if (!remove_dependency_line(&lines, name)) {
        forge_util_set_error(error, error_size,
                  "dependency '%s' is parsed but its line was not found in %s; "
                  "fix the file by hand", name, manifest_path);
        line_list_free(&lines);
        goto done;
    }
    if (write_lines(manifest_path, &lines, error, error_size) != 0) {
        line_list_free(&lines);
        goto done;
    }
    line_list_free(&lines);
    printf("forge: removed %s from %s\n", name, manifest_path);

    /* Dropping the pin from Forge.lock happens through the normal resolver. */
    if (resolve_after_edit(manifest_path, logger, error, error_size) != 0) {
        goto done;
    }
    printf("forge: updated Forge.lock\n");
    status = 0;
done:
    free(manifest);
    return status;
}
