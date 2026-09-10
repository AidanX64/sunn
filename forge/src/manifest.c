#ifdef _MSC_VER
#define _CRT_SECURE_NO_WARNINGS
#endif

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "forge/manifest.h"
#include "forge_util.h"

#define FORGE_MANIFEST_LINE_MAX 4096U

typedef enum ForgeManifestSection {
    FORGE_SECTION_NONE,
    FORGE_SECTION_PROJECT,
    FORGE_SECTION_SOURCES,
    FORGE_SECTION_TARGETS,
    FORGE_SECTION_BUILD,
    FORGE_SECTION_DEPENDENCIES,
    FORGE_SECTION_PROFILE_DEBUG,
    FORGE_SECTION_PROFILE_RELEASE
} ForgeManifestSection;

/* One key = "value" pair inside an inline table ({ git = "...", tag = "..." }). */
#define FORGE_INLINE_KEY_MAX 32U
#define FORGE_INLINE_MAX_ENTRIES 8U

typedef struct ForgeInlineEntry {
    char key[FORGE_INLINE_KEY_MAX];
    char value[FORGE_MANIFEST_VALUE_MAX];
} ForgeInlineEntry;

typedef struct ForgeProfileSeen {
    int opt_level;
    int debug_info;
    int warnings_as_errors;
    int std_version;
    int cflags;
} ForgeProfileSeen;

typedef struct ForgeManifestSeen {
    int project_name;
    int project_version;
    int source_c;
    int source_cpp;
    int source_asm;
    int target_os;
    int target_arch;
    int compiler;
    ForgeProfileSeen debug;
    ForgeProfileSeen release;
} ForgeManifestSeen;

static void remove_comment(char *text)
{
    int quoted = 0;
    int escaped = 0;
    char *cursor;

    for (cursor = text; *cursor != '\0'; ++cursor) {
        if (escaped) {
            escaped = 0;
        } else if (*cursor == '\\' && quoted) {
            escaped = 1;
        } else if (*cursor == '"') {
            quoted = !quoted;
        } else if (*cursor == '#' && !quoted) {
            *cursor = '\0';
            return;
        }
    }
}

static int parse_string(char **cursor, char *destination, size_t destination_size,
                        char *error, size_t error_size)
{
    char *source = *cursor;
    size_t length = 0U;

    if (*source != '"') {
        forge_util_set_error(error, error_size, "expected a quoted string");
        return -1;
    }

    ++source;
    while (*source != '\0' && *source != '"') {
        char character = *source++;

        if (character == '\\') {
            if (*source != '"' && *source != '\\') {
                forge_util_set_error(error, error_size,
                          "only \\\" and \\\\ escapes are supported");
                return -1;
            }
            character = *source++;
        }

        if (length + 1U >= destination_size) {
            forge_util_set_error(error, error_size, "string exceeds %u characters",
                      (unsigned int)(destination_size - 1U));
            return -1;
        }
        destination[length++] = character;
    }

    if (*source != '"') {
        forge_util_set_error(error, error_size, "unterminated quoted string");
        return -1;
    }

    destination[length] = '\0';
    *cursor = source + 1;
    return 0;
}

static int parse_string_list(char *value, ForgeStringList *list,
                             char *error, size_t error_size)
{
    char *cursor = forge_util_trim(value);

    if (*cursor != '[') {
        forge_util_set_error(error, error_size, "expected a string array starting with '['");
        return -1;
    }

    ++cursor;
    cursor = forge_util_trim(cursor);
    list->count = 0U;
    if (*cursor == ']') {
        cursor = forge_util_trim(cursor + 1);
        if (*cursor != '\0') {
            forge_util_set_error(error, error_size, "unexpected text after string array");
            return -1;
        }
        return 0;
    }

    for (;;) {
        if (list->count == FORGE_MANIFEST_MAX_ITEMS) {
            forge_util_set_error(error, error_size, "array has more than %u entries",
                      FORGE_MANIFEST_MAX_ITEMS);
            return -1;
        }
        if (parse_string(&cursor, list->items[list->count],
                         sizeof(list->items[list->count]), error, error_size) != 0) {
            return -1;
        }
        if (list->items[list->count][0] == '\0') {
            forge_util_set_error(error, error_size, "empty strings are not allowed in arrays");
            return -1;
        }
        ++list->count;

        cursor = forge_util_trim(cursor);
        if (*cursor == ']') {
            cursor = forge_util_trim(cursor + 1);
            if (*cursor != '\0') {
                forge_util_set_error(error, error_size, "unexpected text after string array");
                return -1;
            }
            return 0;
        }
        if (*cursor != ',') {
            forge_util_set_error(error, error_size, "expected ',' or ']' in string array");
            return -1;
        }
        cursor = forge_util_trim(cursor + 1);
        if (*cursor == ']') {
            forge_util_set_error(error, error_size, "trailing commas are not allowed in arrays");
            return -1;
        }
    }
}

static ForgeManifestSection parse_section(const char *text)
{
    if (strcmp(text, "[project]") == 0) {
        return FORGE_SECTION_PROJECT;
    }
    if (strcmp(text, "[sources]") == 0) {
        return FORGE_SECTION_SOURCES;
    }
    if (strcmp(text, "[targets]") == 0) {
        return FORGE_SECTION_TARGETS;
    }
    if (strcmp(text, "[build]") == 0) {
        return FORGE_SECTION_BUILD;
    }
    if (strcmp(text, "[dependencies]") == 0) {
        return FORGE_SECTION_DEPENDENCIES;
    }
    if (strcmp(text, "[profile.debug]") == 0) {
        return FORGE_SECTION_PROFILE_DEBUG;
    }
    if (strcmp(text, "[profile.release]") == 0) {
        return FORGE_SECTION_PROFILE_RELEASE;
    }
    return FORGE_SECTION_NONE;
}

static int parse_scalar(char *value, char *destination, size_t destination_size,
                        char *error, size_t error_size)
{
    char *cursor = forge_util_trim(value);

    if (parse_string(&cursor, destination, destination_size, error, error_size) != 0) {
        return -1;
    }
    cursor = forge_util_trim(cursor);
    if (*cursor != '\0') {
        forge_util_set_error(error, error_size, "unexpected text after quoted string");
        return -1;
    }
    if (destination[0] == '\0') {
        forge_util_set_error(error, error_size, "empty strings are not allowed");
        return -1;
    }
    return 0;
}

static int reject_duplicate(int *seen, const char *key, char *error, size_t error_size)
{
    if (*seen) {
        forge_util_set_error(error, error_size, "duplicate field '%s'", key);
        return -1;
    }
    *seen = 1;
    return 0;
}

static int parse_integer(char *value, int minimum, int maximum, int *output,
                         char *error, size_t error_size)
{
    char *cursor = forge_util_trim(value);
    char *end = NULL;
    long parsed;

    if (*cursor == '\0') {
        forge_util_set_error(error, error_size, "expected an integer");
        return -1;
    }
    parsed = strtol(cursor, &end, 10);
    if (end == cursor || *forge_util_trim(end) != '\0' ||
        parsed < minimum || parsed > maximum) {
        forge_util_set_error(error, error_size,
                  "expected an integer between %d and %d", minimum, maximum);
        return -1;
    }
    *output = (int)parsed;
    return 0;
}

static int parse_boolean(char *value, int *output, char *error, size_t error_size)
{
    char *cursor = forge_util_trim(value);

    if (strcmp(cursor, "true") == 0) {
        *output = 1;
        return 0;
    }
    if (strcmp(cursor, "false") == 0) {
        *output = 0;
        return 0;
    }
    forge_util_set_error(error, error_size, "expected 'true' or 'false'");
    return -1;
}

/* Dependency names become include paths and link inputs, so restrict them to
 * the same portable ASCII charset forge uses everywhere else (explicit ranges
 * rather than isalnum, which is locale-dependent). */
static int dependency_name_is_valid(const char *name)
{
    size_t index;

    for (index = 0U; name[index] != '\0'; ++index) {
        unsigned char character = (unsigned char)name[index];

        if (!((character >= 'a' && character <= 'z') ||
              (character >= 'A' && character <= 'Z') ||
              (character >= '0' && character <= '9') ||
              character == '-' || character == '_' || character == '.')) {
            return 0;
        }
    }
    return name[0] != '\0';
}

/*
 * Versions follow the semver core shape: three numeric components without
 * leading zeros, optionally followed by a '-' pre-release of alphanumeric,
 * hyphen, or dot characters ("1.2.3", "1.2.3-rc.1"). Strictness here means
 * the version can be embedded as a C string literal and later sorted or
 * compared mechanically without edge cases.
 */
static int version_is_valid(const char *version)
{
    size_t index = 0U;
    unsigned int component;

    for (component = 0U; component < 3U; ++component) {
        if (component > 0U) {
            if (version[index] != '.') {
                return 0;
            }
            ++index;
        }
        if (!isdigit((unsigned char)version[index])) {
            return 0;
        }
        /* "0" alone is valid; "01" is not. */
        if (version[index] == '0' && isdigit((unsigned char)version[index + 1U])) {
            return 0;
        }
        while (isdigit((unsigned char)version[index])) {
            ++index;
        }
    }
    if (version[index] == '\0') {
        return 1;
    }
    if (version[index] != '-') {
        return 0;
    }
    ++index;
    if (version[index] == '\0') {
        return 0;
    }
    for (; version[index] != '\0'; ++index) {
        unsigned char character = (unsigned char)version[index];

        if (!((character >= 'a' && character <= 'z') ||
              (character >= 'A' && character <= 'Z') ||
              (character >= '0' && character <= '9') ||
              character == '-' || character == '.')) {
            return 0;
        }
    }
    return 1;
}

/*
 * Parses a strict inline table: '{' key = "string" (, key = "string")* '}'.
 * Bare keys are short identifiers; values are quoted strings; trailing
 * commas are rejected like everywhere else in the manifest.
 */
static int parse_inline_table(char *value, ForgeInlineEntry *entries,
                              size_t max_entries, size_t *entry_count,
                              char *error, size_t error_size)
{
    char *cursor = forge_util_trim(value);
    size_t count = 0U;

    *entry_count = 0U;
    if (*cursor != '{') {
        forge_util_set_error(error, error_size,
                  "expected an inline table starting with '{'");
        return -1;
    }
    ++cursor;
    cursor = forge_util_trim(cursor);
    if (*cursor == '}') {
        cursor = forge_util_trim(cursor + 1);
        if (*cursor != '\0') {
            forge_util_set_error(error, error_size, "unexpected text after inline table");
            return -1;
        }
        return 0;
    }
    for (;;) {
        ForgeInlineEntry *entry;
        char *key_start;

        if (count == max_entries) {
            forge_util_set_error(error, error_size, "inline table has too many entries");
            return -1;
        }
        entry = &entries[count];
        key_start = cursor;
        while (isalnum((unsigned char)*cursor) || *cursor == '-' || *cursor == '_' ||
               *cursor == '.') {
            ++cursor;
        }
        if (cursor == key_start) {
            forge_util_set_error(error, error_size, "expected a key in inline table");
            return -1;
        }
        if ((size_t)(cursor - key_start) >= FORGE_INLINE_KEY_MAX) {
            forge_util_set_error(error, error_size, "inline table key is too long");
            return -1;
        }
        memcpy(entry->key, key_start, (size_t)(cursor - key_start));
        entry->key[cursor - key_start] = '\0';
        cursor = forge_util_trim(cursor);
        if (*cursor != '=') {
            forge_util_set_error(error, error_size,
                      "expected '=' after '%s' in inline table", entry->key);
            return -1;
        }
        cursor = forge_util_trim(cursor + 1);
        if (parse_string(&cursor, entry->value, sizeof(entry->value),
                         error, error_size) != 0) {
            return -1;
        }
        ++count;
        cursor = forge_util_trim(cursor);
        if (*cursor == '}') {
            cursor = forge_util_trim(cursor + 1);
            if (*cursor != '\0') {
                forge_util_set_error(error, error_size, "unexpected text after inline table");
                return -1;
            }
            *entry_count = count;
            return 0;
        }
        if (*cursor != ',') {
            forge_util_set_error(error, error_size,
                      "expected ',' or '}' in inline table");
            return -1;
        }
        cursor = forge_util_trim(cursor + 1);
        if (*cursor == '}') {
            forge_util_set_error(error, error_size,
                      "trailing commas are not allowed in inline tables");
            return -1;
        }
    }
}

static const ForgeInlineEntry *find_inline_entry(const ForgeInlineEntry *entries,
                                                 size_t count, const char *key)
{
    size_t index;

    for (index = 0U; index < count; ++index) {
        if (strcmp(entries[index].key, key) == 0) {
            return &entries[index];
        }
    }
    return NULL;
}

static int parse_dependency_assignment(ForgeDependencyList *list, const char *name,
                                       char *value, char *error, size_t error_size)
{
    ForgeInlineEntry entries[FORGE_INLINE_MAX_ENTRIES];
    ForgeDependency *dependency;
    const ForgeInlineEntry *entry;
    const ForgeInlineEntry *git_entry;
    size_t count = 0U;
    size_t index;

    if (!dependency_name_is_valid(name)) {
        forge_util_set_error(error, error_size,
                  "'%s' is not a valid dependency name; use letters, digits, '-', '_', '.'",
                  name);
        return -1;
    }
    if (strlen(name) >= FORGE_MANIFEST_VALUE_MAX) {
        forge_util_set_error(error, error_size,
                  "dependency name is too long (%u characters maximum)",
                  (unsigned int)(FORGE_MANIFEST_VALUE_MAX - 1U));
        return -1;
    }
    for (index = 0U; index < list->count; ++index) {
        if (strcmp(list->items[index].name, name) == 0) {
            forge_util_set_error(error, error_size, "duplicate dependency '%s'", name);
            return -1;
        }
    }
    if (parse_inline_table(value, entries, FORGE_INLINE_MAX_ENTRIES, &count,
                           error, error_size) != 0) {
        return -1;
    }
    if (count == 0U) {
        forge_util_set_error(error, error_size,
                  "dependency '%s' needs a 'path', 'git', or 'registry' source", name);
        return -1;
    }
    entry = find_inline_entry(entries, count, "path");
    git_entry = find_inline_entry(entries, count, "git");
    {
        const ForgeInlineEntry *registry_entry =
            find_inline_entry(entries, count, "registry");
        const ForgeInlineEntry *version_entry =
            find_inline_entry(entries, count, "version");
        int source_count = (entry != NULL) + (git_entry != NULL) +
                           (registry_entry != NULL);

        if (source_count != 1) {
            forge_util_set_error(error, error_size,
                      "dependency '%s' needs exactly one source: 'path', "
                      "'git', or 'registry'", name);
            return -1;
        }
        if (version_entry != NULL && registry_entry == NULL) {
            forge_util_set_error(error, error_size,
                      "dependency '%s': version only applies to registry "
                      "dependencies", name);
            return -1;
        }
        if (registry_entry != NULL) {
            if (find_inline_entry(entries, count, "tag") != NULL ||
                find_inline_entry(entries, count, "branch") != NULL ||
                find_inline_entry(entries, count, "rev") != NULL) {
                forge_util_set_error(error, error_size,
                          "dependency '%s': refs only apply to git dependencies",
                          name);
                return -1;
            }
            if (find_inline_entry(entries, count, "submodules") != NULL) {
                forge_util_set_error(error, error_size,
                          "dependency '%s': submodules only apply to git "
                          "dependencies", name);
                return -1;
            }
        }
    }
    if (find_inline_entry(entries, count, "submodules") != NULL && entry != NULL) {
        forge_util_set_error(error, error_size,
                  "dependency '%s': submodules only apply to git dependencies",
                  name);
        return -1;
    }
    if (list->count == FORGE_MANIFEST_MAX_DEPS) {
        forge_util_set_error(error, error_size, "more than %u dependencies",
                  FORGE_MANIFEST_MAX_DEPS);
        return -1;
    }
    dependency = &list->items[list->count];
    (void)snprintf(dependency->name, sizeof(dependency->name), "%s", name);
    dependency->git_url[0] = '\0';
    dependency->ref[0] = '\0';
    dependency->path[0] = '\0';
    dependency->registry[0] = '\0';
    dependency->registry_version[0] = '\0';
    if (entry != NULL) {
        if (find_inline_entry(entries, count, "tag") != NULL ||
            find_inline_entry(entries, count, "branch") != NULL ||
            find_inline_entry(entries, count, "rev") != NULL) {
            forge_util_set_error(error, error_size,
                      "dependency '%s': refs only apply to git dependencies", name);
            return -1;
        }
        if (find_inline_entry(entries, count, "version") != NULL) {
            forge_util_set_error(error, error_size,
                      "dependency '%s': version only applies to registry "
                      "dependencies", name);
            return -1;
        }
        (void)snprintf(dependency->path, sizeof(dependency->path), "%s", entry->value);
    } else if (git_entry != NULL) {
        static const char *const ref_keys[] = { "tag", "branch", "rev" };
        const ForgeInlineEntry *submodules_entry;
        size_t key_index;

        (void)snprintf(dependency->git_url, sizeof(dependency->git_url), "%s",
                       git_entry->value);
        submodules_entry = find_inline_entry(entries, count, "submodules");
        if (submodules_entry != NULL) {
            char flag_value[FORGE_MANIFEST_VALUE_MAX];

            /* parse_boolean trims in place, so it needs a writable copy. */
            (void)snprintf(flag_value, sizeof(flag_value), "%s",
                           submodules_entry->value);
            if (parse_boolean(flag_value, &dependency->submodules,
                              error, error_size) != 0) {
                forge_util_set_error(error, error_size,
                          "dependency '%s': submodules must be \"true\" or \"false\"",
                          name);
                return -1;
            }
        }
        for (key_index = 0U; key_index < 3U; ++key_index) {
            entry = find_inline_entry(entries, count, ref_keys[key_index]);
            if (entry == NULL) {
                continue;
            }
            if (dependency->ref[0] != '\0') {
                forge_util_set_error(error, error_size,
                          "dependency '%s' has multiple refs; use one of tag/branch/rev",
                          name);
                return -1;
            }
            (void)snprintf(dependency->ref, sizeof(dependency->ref), "%s", entry->value);
        }
    } else {
        /* The source-count check above guarantees a registry entry here. */
        const ForgeInlineEntry *registry_entry =
            find_inline_entry(entries, count, "registry");
        const ForgeInlineEntry *version_entry =
            find_inline_entry(entries, count, "version");

        (void)snprintf(dependency->registry, sizeof(dependency->registry),
                       "%s", registry_entry->value);
        if (version_entry != NULL) {
            (void)snprintf(dependency->registry_version,
                           sizeof(dependency->registry_version), "%s",
                           version_entry->value);
        }
    }
    /* Reject unknown keys so typos fail loudly. */
    for (index = 0U; index < count; ++index) {
        static const char *const allowed[] = {
            "path", "git", "tag", "branch", "rev", "submodules",
            "registry", "version"
        };
        size_t allowed_index;
        int known = 0;

        for (allowed_index = 0U; allowed_index < 8U; ++allowed_index) {
            if (strcmp(entries[index].key, allowed[allowed_index]) == 0) {
                known = 1;
                break;
            }
        }
        if (!known) {
            forge_util_set_error(error, error_size,
                      "unknown field '%s' in dependency '%s'", entries[index].key, name);
            return -1;
        }
    }
    ++list->count;
    return 0;
}

/* Applies a profile section assignment, sharing the parser between
 * [profile.debug] and [profile.release]. */
static int parse_profile_assignment(ForgeProfileSeen *seen, ForgeBuildProfile *profile,
                                    const char *key, char *value,
                                    char *error, size_t error_size)
{
    if (strcmp(key, "opt-level") == 0) {
        return reject_duplicate(&seen->opt_level, key, error, error_size) ||
               parse_integer(value, 0, 3, &profile->opt_level, error, error_size);
    }
    if (strcmp(key, "debug") == 0) {
        return reject_duplicate(&seen->debug_info, key, error, error_size) ||
               parse_boolean(value, &profile->debug_info, error, error_size);
    }
    if (strcmp(key, "warnings-as-errors") == 0) {
        return reject_duplicate(&seen->warnings_as_errors, key, error, error_size) ||
               parse_boolean(value, &profile->warnings_as_errors, error, error_size);
    }
    if (strcmp(key, "std") == 0) {
        return reject_duplicate(&seen->std_version, key, error, error_size) ||
               parse_scalar(value, profile->std_version, sizeof(profile->std_version),
                            error, error_size);
    }
    if (strcmp(key, "cflags") == 0) {
        return reject_duplicate(&seen->cflags, key, error, error_size) ||
               parse_string_list(value, &profile->cflags, error, error_size);
    }
    forge_util_set_error(error, error_size, "field '%s' is not allowed in this section", key);
    return -1;
}

static int parse_assignment(ForgeManifestSection section, char *line,
                            ForgeManifest *manifest, ForgeManifestSeen *seen,
                            char *error, size_t error_size)
{
    char *equals = strchr(line, '=');
    char *key;
    char *value;

    if (equals == NULL) {
        forge_util_set_error(error, error_size, "expected 'key = value'");
        return -1;
    }
    *equals = '\0';
    key = forge_util_trim(line);
    value = forge_util_trim(equals + 1);
    if (*key == '\0' || *value == '\0') {
        forge_util_set_error(error, error_size, "expected a non-empty key and value");
        return -1;
    }

    if (section == FORGE_SECTION_PROJECT && strcmp(key, "name") == 0) {
        return reject_duplicate(&seen->project_name, key, error, error_size) ||
               parse_scalar(value, manifest->project_name, sizeof(manifest->project_name),
                            error, error_size);
    }
    if (section == FORGE_SECTION_PROJECT && strcmp(key, "version") == 0) {
        char parsed[FORGE_MANIFEST_VALUE_MAX];

        if (reject_duplicate(&seen->project_version, key, error, error_size) != 0) {
            return -1;
        }
        if (parse_scalar(value, parsed, sizeof(parsed), error, error_size) != 0) {
            return -1;
        }
        if (!version_is_valid(parsed)) {
            forge_util_set_error(error, error_size,
                      "version '%s' must be MAJOR.MINOR.PATCH with an optional "
                      "-prerelease (e.g. \"1.2.3\" or \"1.2.3-rc.1\")", parsed);
            return -1;
        }
        (void)snprintf(manifest->project_version,
                       sizeof(manifest->project_version), "%s", parsed);
        return 0;
    }
    if (section == FORGE_SECTION_SOURCES && strcmp(key, "c") == 0) {
        return reject_duplicate(&seen->source_c, key, error, error_size) ||
               parse_string_list(value, &manifest->c_source_dirs, error, error_size);
    }
    if (section == FORGE_SECTION_SOURCES && strcmp(key, "cpp") == 0) {
        return reject_duplicate(&seen->source_cpp, key, error, error_size) ||
               parse_string_list(value, &manifest->cpp_source_dirs, error, error_size);
    }
    if (section == FORGE_SECTION_SOURCES && strcmp(key, "asm") == 0) {
        return reject_duplicate(&seen->source_asm, key, error, error_size) ||
               parse_string_list(value, &manifest->asm_source_dirs, error, error_size);
    }
    if (section == FORGE_SECTION_TARGETS && strcmp(key, "os") == 0) {
        return reject_duplicate(&seen->target_os, key, error, error_size) ||
               parse_string_list(value, &manifest->target_os, error, error_size);
    }
    if (section == FORGE_SECTION_TARGETS && strcmp(key, "arch") == 0) {
        return reject_duplicate(&seen->target_arch, key, error, error_size) ||
               parse_string_list(value, &manifest->target_arch, error, error_size);
    }
    if (section == FORGE_SECTION_BUILD && strcmp(key, "compiler") == 0) {
        return reject_duplicate(&seen->compiler, key, error, error_size) ||
               parse_scalar(value, manifest->compiler_override,
                            sizeof(manifest->compiler_override), error, error_size);
    }
    if (section == FORGE_SECTION_DEPENDENCIES) {
        return parse_dependency_assignment(&manifest->dependencies, key, value,
                                           error, error_size);
    }
    if (section == FORGE_SECTION_PROFILE_DEBUG) {
        return parse_profile_assignment(&seen->debug, &manifest->debug_profile,
                                        key, value, error, error_size);
    }
    if (section == FORGE_SECTION_PROFILE_RELEASE) {
        return parse_profile_assignment(&seen->release, &manifest->release_profile,
                                        key, value, error, error_size);
    }

    forge_util_set_error(error, error_size, "field '%s' is not allowed in this section", key);
    return -1;
}
int forge_manifest_load(const char *path, ForgeManifest *manifest,
                        char *error, size_t error_size)
{
    FILE *file;
    char line[FORGE_MANIFEST_LINE_MAX];
    unsigned long line_number = 0UL;
    unsigned int sections_seen = 0U;
    ForgeManifestSection section = FORGE_SECTION_NONE;
    ForgeManifestSeen seen = {0};

    if (path == NULL || manifest == NULL) {
        forge_util_set_error(error, error_size, "manifest path and output are required");
        return -1;
    }
    *manifest = (ForgeManifest){0};
    manifest->debug_profile.opt_level = -1;
    manifest->release_profile.opt_level = -1;
    /* Binary mode keeps ftell exact, which the embedded-NUL check below
     * relies on; \r\n endings are handled by the trim helpers instead. */
    file = fopen(path, "rb");
    if (file == NULL) {
        forge_util_set_error(error, error_size, "could not open manifest '%s'", path);
        return -1;
    }

    /*
     * A UTF-8 byte-order mark is accepted (editors on Windows add one
     * silently) but must be skipped before parsing; otherwise the first
     * section header would read as "\xEF\xBB\xBF[project]" and fail with a
     * confusing "unknown section".
     */
    {
        char prefix[3];
        size_t prefix_length = fread(prefix, 1U, sizeof(prefix), file);

        if (prefix_length == 3U && (unsigned char)prefix[0] == 0xEFU &&
            (unsigned char)prefix[1] == 0xBBU && (unsigned char)prefix[2] == 0xBFU) {
            /* BOM consumed; parsing continues after it. */
        } else if (fseek(file, 0L, SEEK_SET) != 0) {
            forge_util_set_error(error, error_size,
                      "could not rewind manifest '%s'", path);
            (void)fclose(file);
            return -1;
        }
    }

    while (fgets(line, sizeof(line), file) != NULL) {
        char *text;
        long position_before = ftell(file) - (long)strlen(line);
        long bytes_read = ftell(file) - position_before;

        ++line_number;
        /*
         * strlen must account for every byte fgets consumed; when it does
         * not, an embedded NUL truncated the C string silently — reject
         * binary contamination loudly instead of parsing half a line.
         */
        if ((long)strlen(line) != bytes_read || bytes_read <= 0L) {
            forge_util_set_error(error, error_size,
                      "%s:%lu: line contains an embedded NUL byte or invalid "
                      "binary content", path, line_number);
            (void)fclose(file);
            return -1;
        }
        if (strchr(line, '\n') == NULL && !feof(file)) {
            forge_util_set_error(error, error_size, "%s:%lu: line is too long", path, line_number);
            (void)fclose(file);
            return -1;
        }
        remove_comment(line);
        text = forge_util_trim(line);
        if (*text == '\0') {
            continue;
        }
        if (*text == '[') {
            unsigned int section_bit = 1U << (unsigned int)parse_section(text);

            section = parse_section(text);
            if (section == FORGE_SECTION_NONE) {
                forge_util_set_error(error, error_size, "%s:%lu: unknown section '%s'", path,
                          line_number, text);
                (void)fclose(file);
                return -1;
            }
            /*
             * TOML forbids repeating a table; silently merging two
             * [dependencies] blocks (say, one hand-written and one appended
             * by `forge add`) hides exactly the edits users need to see.
             */
            if ((sections_seen & section_bit) != 0U) {
                forge_util_set_error(error, error_size, "%s:%lu: duplicate section '%s'",
                          path, line_number, text);
                (void)fclose(file);
                return -1;
            }
            sections_seen |= section_bit;
            continue;
        }
        if (section == FORGE_SECTION_NONE) {
            forge_util_set_error(error, error_size, "%s:%lu: field without a section header; "
                                  "expected a [section] line first", path, line_number);
            (void)fclose(file);
            return -1;
        }
        if (parse_assignment(section, text, manifest, &seen, error, error_size) != 0) {
            char detail[FORGE_MANIFEST_LINE_MAX];
            if (error == NULL || error[0] == '\0') {
                (void)snprintf(detail, sizeof(detail), "invalid field");
            } else {
                (void)snprintf(detail, sizeof(detail), "%s", error);
            }
            forge_util_set_error(error, error_size, "%s:%lu: %s", path, line_number, detail);
            (void)fclose(file);
            return -1;
        }
    }
    (void)fclose(file);

    if (!seen.project_name || !seen.target_os || !seen.target_arch) {
        forge_util_set_error(error, error_size,
                  "%s: manifest requires project.name and targets.os/arch", path);
        return -1;
    }
    if (manifest->c_source_dirs.count == 0U && manifest->cpp_source_dirs.count == 0U &&
        manifest->asm_source_dirs.count == 0U) {
        forge_util_set_error(error, error_size, "%s: at least one source directory is required", path);
        return -1;
    }
    if (manifest->target_os.count == 0U || manifest->target_arch.count == 0U) {
        forge_util_set_error(error, error_size, "%s: targets.os and targets.arch cannot be empty", path);
        return -1;
    }
    return 0;
}
