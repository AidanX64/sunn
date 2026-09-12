#include "forge/registry.h"

#include "forge/platform.h"

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "forge/argv.h"
#include "forge/deps.h"
#include "forge/fetch.h"
#include "forge/process.h"
#include "forge_util.h"

static int read_response_file(const char *path, char *body, size_t body_size,
                               char *error, size_t error_size);
static int resolve_floor(ForgeLogger *logger, const char *dep_name,
                         const char *base, const char *package,
                         const char *min_version, const char *tmp_path,
                         int offline, char *floor_version, size_t floor_size,
                         unsigned *floor_revision,
                         char *error, size_t error_size);

static int read_response_file(const char *path, char *body, size_t body_size,
                               char *error, size_t error_size);
static int resolve_floor(ForgeLogger *logger, const char *dep_name,
                         const char *base, const char *package,
                         const char *min_version, const char *tmp_path,
                         int offline, char *floor_version, size_t floor_size,
                         unsigned *floor_revision,
                         char *error, size_t error_size);
static int registry_list_versions(ForgeLogger *logger, const char *base,
                                  const char *package, const char *tmp_path,
                                  char versions[][FORGE_MANIFEST_VALUE_MAX],
                                  unsigned *revisions, size_t capacity,
                                  size_t *count, char *error,
                                  size_t error_size);

/* Forward declarations for JSON helpers + recipe parser defined further
 * below; overlay/index code above needs them. */
typedef struct ForgeJsonCursor {
    const char *text;
    const char *error;
} ForgeJsonCursor;
static void json_skip_space(ForgeJsonCursor *cursor);
static int json_read_string(ForgeJsonCursor *cursor, char *out,
                            size_t out_size);
static int json_skip_value(ForgeJsonCursor *cursor);
static int json_object_string(ForgeJsonCursor *cursor, const char *wanted,
                              char *out, size_t out_size, int *is_null);
static int json_object_uint(ForgeJsonCursor *cursor, const char *wanted,
                            unsigned *out, unsigned max, int *is_null);
static int json_array_select(const char *json, const char *array_key,
                             const char *match_key, const char *match_value,
                             const char *want_a, char *out_a, size_t a_size,
                             const char *want_b, char *out_b, size_t b_size);
static int parse_recipe(const char *body, const char *base,
                        ForgeRegistryPin *pin, char *error, size_t error_size);
static int version_text_is_valid(const char *version);

/* ------------------------------------------------------------------ */
/* Overlays: local site roots that shadow the registry                 */
/*                                                                     */
/* FORGE_OVERLAYS names one or more site roots (same static layout as  */
/* file:// registries: packages/sunn.registry.json +                   */
/* packages/<name>/<version>.json + tarballs). Separators are ';' on   */
/* Windows and ':' elsewhere; surrounding spaces are ignored. An       */
/* overlay wins over the configured registry for both recipe queries   */
/* and version listings, first overlay wins. Paths that escape the     */
/* overlay root (..) are refused loudly.                               */
/* ------------------------------------------------------------------ */

static int overlay_roots(char roots[][FORGE_PATH_MAX], size_t capacity,
                         size_t *count)
{
    const char *env = getenv("FORGE_OVERLAYS");
#if FORGE_PLATFORM_WINDOWS
    const char sep = ';';
#else
    const char sep = ':';
#endif
    const char *p;

    *count = 0U;
    if (env == NULL || env[0] == '\0') {
        return 0;
    }
    p = env;
    for (;;) {
        const char *end;
        size_t len;

        while (*p == ' ' || *p == '\t') {
            ++p;
        }
        if (*p == '\0') {
            return 0;
        }
        end = strchr(p, sep);
        len = end != NULL ? (size_t)(end - p) : strlen(p);
        while (len != 0U && (p[len - 1U] == ' ' || p[len - 1U] == '\t')) {
            --len;
        }
        if (len != 0U) {
            if (len >= FORGE_PATH_MAX || *count == capacity) {
                return -1;
            }
            memcpy(roots[*count], p, len);
            roots[*count][len] = '\0';
            if (strstr(roots[*count], "..") != NULL) {
                return -1;
            }
            ++*count;
        }
        if (end == NULL) {
            return 0;
        }
        p = end + 1U;
    }
}

/* True when <root>/packages/<package>/<version>.json exists and parses as
 * a recipe for that package/version; fills pin/defs like the file query. */
static int overlay_try_recipe(const char *root, const char *package,
                              const char *version,
                              ForgeRegistryPin *pin, ForgeFeatureDefs *defs,
                              char *error, size_t error_size)
{
    char path[FORGE_PATH_MAX];
    char body[65536];
    FILE *probe;

    if (strchr(package, '/') != NULL || strchr(package, '\\') != NULL ||
        strstr(package, "..") != NULL || strchr(version, '/') != NULL ||
        strchr(version, '\\') != NULL || strstr(version, "..") != NULL) {
        return 0;
    }
    if ((size_t)snprintf(path, sizeof(path), "%s/packages/%s/%s.json", root,
                         package, version) >= sizeof(path)) {
        return 0;
    }
    probe = fopen(path, "rb");
    if (probe == NULL) {
        return 0;
    }
    (void)fclose(probe);
    if (read_response_file(path, body, sizeof(body), error, error_size) != 0) {
        return 0;
    }
    if (body[0] != '{') {
        return 0;
    }
    {
        char base[FORGE_PATH_MAX];

        /* Relative tarball locations inside an overlay resolve against a
         * file:// view of the overlay root so cached bytes stay local. */
        if ((size_t)snprintf(base, sizeof(base), "file://%s", root) >=
            sizeof(base)) {
            return 0;
        }
        if (parse_recipe(body, base, pin, error, error_size) != 0) {
            return 0;
        }
        if (forge_registry_parse_features(body, defs, error,
                                          error_size) != 0) {
            return 0;
        }
    }
    return 1;
}

/* ------------------------------------------------------------------ */
/* Index version listing (range resolution)                            */
/*                                                                     */
/* Ranges need the full version list, not just latest. Both transports */
/* expose the same static index shape (packages[] with versions[]), so */
/* one parser serves file:// roots, overlay roots, and the HTTP index  */
/* document fetched to tmp_path. Bounded to                              */
/* FORGE_REGISTRY_MAX_LISTED_VERSIONS entries.                         */
/* ------------------------------------------------------------------ */

static int index_package_span(const char *body, const char *package,
                              char *span_out, size_t span_size)
{
    ForgeJsonCursor cursor = { body, NULL };
    int first = 1;
    int found_array = 0;

    json_skip_space(&cursor);
    if (*cursor.text != '{') {
        return -1;
    }
    ++cursor.text;
    for (;;) {
        char key[128];

        json_skip_space(&cursor);
        if (*cursor.text == '}') {
            return 0;
        }
        if (!first) {
            if (*cursor.text != ',') {
                return -1;
            }
            ++cursor.text;
            json_skip_space(&cursor);
        }
        first = 0;
        if (json_read_string(&cursor, key, sizeof(key)) != 0) {
            return -1;
        }
        json_skip_space(&cursor);
        if (*cursor.text != ':') {
            return -1;
        }
        ++cursor.text;
        json_skip_space(&cursor);
        if (strcmp(key, "packages") == 0) {
            found_array = 1;
            break;
        }
        if (json_skip_value(&cursor) != 0) {
            return -1;
        }
    }
    if (!found_array || *cursor.text != '[') {
        return 0;
    }
    ++cursor.text;
    for (;;) {
        const char *span_start;
        size_t span_length;
        ForgeJsonCursor object;
        char candidate[FORGE_PATH_MAX];
        int is_null = 0;
        int matched;

        json_skip_space(&cursor);
        if (*cursor.text == ']') {
            return 0;
        }
        if (*cursor.text != '{') {
            return -1;
        }
        span_start = cursor.text;
        if (json_skip_value(&cursor) != 0) {
            return -1;
        }
        span_length = (size_t)(cursor.text - span_start);
        if (span_length >= span_size) {
            return -1;
        }
        memcpy(span_out, span_start, span_length);
        span_out[span_length] = '\0';
        object.text = span_out;
        object.error = NULL;
        matched = json_object_string(&object, "name", candidate,
                                     sizeof(candidate), &is_null);
        if (matched < 0) {
            return -1;
        }
        if (matched != 0 && !is_null && strcmp(candidate, package) == 0) {
            return 1;
        }
        json_skip_space(&cursor);
        if (*cursor.text == ',') {
            ++cursor.text;
        }
    }
}

static int index_collect_versions(const char *package_span,
                                  char versions[][FORGE_MANIFEST_VALUE_MAX],
                                  unsigned *revisions, size_t capacity,
                                  size_t *count)
{
    ForgeJsonCursor cursor = { package_span, NULL };
    int first = 1;

    *count = 0U;
    json_skip_space(&cursor);
    if (*cursor.text != '{') {
        return -1;
    }
    ++cursor.text;
    for (;;) {
        char key[128];

        json_skip_space(&cursor);
        if (*cursor.text == '}') {
            return 0;
        }
        if (!first) {
            if (*cursor.text != ',') {
                return -1;
            }
            ++cursor.text;
            json_skip_space(&cursor);
        }
        first = 0;
        if (json_read_string(&cursor, key, sizeof(key)) != 0) {
            return -1;
        }
        json_skip_space(&cursor);
        if (*cursor.text != ':') {
            return -1;
        }
        ++cursor.text;
        json_skip_space(&cursor);
        if (strcmp(key, "versions") != 0) {
            if (json_skip_value(&cursor) != 0) {
                return -1;
            }
            continue;
        }
        if (*cursor.text != '[') {
            return -1;
        }
        ++cursor.text;
        for (;;) {
            const char *span_start;
            char span[1024];
            size_t span_length;
            ForgeJsonCursor object;
            char ver[FORGE_MANIFEST_VALUE_MAX];
            unsigned rev = 0U;
            int is_null = 0;
            int matched;

            json_skip_space(&cursor);
            if (*cursor.text == ']') {
                return 0;
            }
            if (*cursor.text != '{') {
                return -1;
            }
            span_start = cursor.text;
            if (json_skip_value(&cursor) != 0) {
                return -1;
            }
            span_length = (size_t)(cursor.text - span_start);
            if (span_length >= sizeof(span)) {
                return -1;
            }
            memcpy(span, span_start, span_length);
            span[span_length] = '\0';
            object.text = span;
            object.error = NULL;
            matched = json_object_string(&object, "version", ver,
                                         sizeof(ver), &is_null);
            if (matched <= 0 || is_null || ver[0] == '\0') {
                return -1;
            }
            object.text = span;
            object.error = NULL;
            matched = json_object_uint(&object, "revision", &rev,
                                       FORGE_REGISTRY_MAX_REVISION,
                                       &is_null);
            if (matched < 0) {
                return -1;
            }
            if (*count < capacity) {
                (void)snprintf(versions[*count], FORGE_MANIFEST_VALUE_MAX,
                               "%s", ver);
                revisions[*count] = (matched > 0 && !is_null) ? rev : 0U;
                ++*count;
            }
            json_skip_space(&cursor);
            if (*cursor.text == ',') {
                ++cursor.text;
            }
        }
    }
}

static int registry_list_versions(ForgeLogger *logger, const char *base,
                                  const char *package, const char *tmp_path,
                                  char versions[][FORGE_MANIFEST_VALUE_MAX],
                                  unsigned *revisions, size_t capacity,
                                  size_t *count, char *error,
                                  size_t error_size)
{
    char body[65536];
    char span[16384];
    int found;

    *count = 0U;
    /* Overlays first: first overlay naming the package wins. */
    {
        char roots[FORGE_REGISTRY_MAX_OVERLAYS][FORGE_PATH_MAX];
        size_t nroots = 0U;
        size_t i;

        if (overlay_roots(roots, FORGE_REGISTRY_MAX_OVERLAYS, &nroots) != 0) {
            forge_util_set_error(error, error_size,
                      "FORGE_OVERLAYS names a path that is too long or "
                      "escapes with '..'");
            return -1;
        }
        for (i = 0U; i < nroots; ++i) {
            char path[FORGE_PATH_MAX];

            if ((size_t)snprintf(path, sizeof(path),
                                 "%s/packages/sunn.registry.json",
                                 roots[i]) >= sizeof(path)) {
                continue;
            }
            {
                FILE *probe = fopen(path, "rb");

                if (probe == NULL) {
                    continue;
                }
                (void)fclose(probe);
            }
            if (read_response_file(path, body, sizeof(body), error,
                                   error_size) != 0) {
                continue;
            }
            if (body[0] != '{') {
                continue;
            }
            found = index_package_span(body, package, span, sizeof(span));
            if (found > 0 &&
                index_collect_versions(span, versions, revisions, capacity,
                                       count) == 0 &&
                *count != 0U) {
                return 0;
            }
        }
    }
    if (strncmp(base, "file://", 7U) == 0) {
        char root[FORGE_PATH_MAX];
        char path[FORGE_PATH_MAX];
        const char *dir = base + 7U;

        if (dir[0] != '/') {
            forge_util_set_error(error, error_size,
                      "file:// registries must point at a local directory "
                      "(file:///path); host shares are not supported");
            return -1;
        }
        if ((size_t)snprintf(root, sizeof(root), "%s", dir) >= sizeof(root)) {
            forge_util_set_error(error, error_size, "registry path is too long");
            return -1;
        }
        if (root[0] == '/' && isalpha((unsigned char)root[1]) &&
            root[2] == ':') {
            memmove(root, root + 1U, strlen(root));
        }
        if ((size_t)snprintf(path, sizeof(path),
                             "%s/packages/sunn.registry.json", root) >=
            sizeof(path)) {
            forge_util_set_error(error, error_size, "registry path is too long");
            return -1;
        }
        if (read_response_file(path, body, sizeof(body), error,
                               error_size) != 0) {
            return -1;
        }
    } else {
        char url[FORGE_PATH_MAX * 2U];
        int written = snprintf(url, sizeof(url), "%s/packages/sunn.registry.json",
                               base);

        if (written < 0 || (size_t)written >= sizeof(url)) {
            forge_util_set_error(error, error_size, "registry query URL is too long");
            return -1;
        }
        if (forge_fetch_url_is_supported(url, error, error_size) != 0 ||
            forge_fetch_to_file(logger, url, tmp_path, error, error_size) != 0 ||
            read_response_file(tmp_path, body, sizeof(body), error,
                               error_size) != 0) {
            return -1;
        }
    }
    if (body[0] != '{') {
        forge_util_set_error(error, error_size,
                  "registry index for '%s' is not valid JSON", package);
        return -1;
    }
    found = index_package_span(body, package, span, sizeof(span));
    if (found < 0) {
        forge_util_set_error(error, error_size,
                  "registry index for '%s' is not valid JSON", package);
        return -1;
    }
    if (found == 0) {
        forge_util_set_error(error, error_size, "registry has no package '%s'",
                  package);
        return -1;
    }
    if (index_collect_versions(span, versions, revisions, capacity, count) != 0) {
        forge_util_set_error(error, error_size,
                  "registry index for '%s' is not valid JSON", package);
        return -1;
    }
    if (*count == 0U) {
        forge_util_set_error(error, error_size,
                  "registry has no versions for '%s'", package);
        return -1;
    }
    return 0;
}

/* Sorts versions descending (newest first), revisions breaking ties. */
static void sort_versions_desc(char versions[][FORGE_MANIFEST_VALUE_MAX],
                               unsigned *revisions, size_t count)
{
    size_t i;
    size_t j;

    for (i = 1U; i < count; ++i) {
        char held_v[FORGE_MANIFEST_VALUE_MAX];
        unsigned held_r = revisions[i];
        size_t at = i;

        (void)snprintf(held_v, sizeof(held_v), "%s", versions[i]);
        while (at > 0U) {
            int order = forge_version_compare(held_v, versions[at - 1U]);

            if (order < 0 ||
                (order == 0 && held_r <= revisions[at - 1U])) {
                break;
            }
            memmove(versions[at], versions[at - 1U],
                    sizeof(versions[at]));
            revisions[at] = revisions[at - 1U];
            --at;
        }
        memmove(versions[at], held_v, sizeof(versions[at]));
        revisions[at] = held_r;
        (void)j;
    }
}

/* ------------------------------------------------------------------ */
/* Base URL + host triplet                                             */
/* ------------------------------------------------------------------ */

int forge_registry_base_url(char *base_out, size_t base_size,
                            char *error, size_t error_size)
{
    const char *base = getenv("FORGE_REGISTRY_URL");
    size_t length;

    if (base == NULL || base[0] == '\0') {
        forge_util_set_error(error, error_size,
                  "registry dependencies need FORGE_REGISTRY_URL to point "
                  "at a sunn registry (e.g. FORGE_REGISTRY_URL=https://sunn.local)");
        return -1;
    }
    if (strncmp(base, "git+", 4U) == 0) {
        forge_util_set_error(error, error_size,
                  "git registries are not served yet (version history lives "
                  "in the static index); use FORGE_OVERLAYS for local ports");
        return -1;
    }
    length = strlen(base);
    while (length > 0U && base[length - 1U] == '/') {
        --length;
    }
    if (length == 0U || length >= base_size) {
        forge_util_set_error(error, error_size,
                  "FORGE_REGISTRY_URL is not usable");
        return -1;
    }
    memcpy(base_out, base, length);
    base_out[length] = '\0';
    return 0;
}
static void registry_identity(const ForgeRegistryPin *pin, char *out,
                               size_t out_size)
{
    size_t used = (size_t)snprintf(out, out_size, "%s|%s|%s|%s|%s|%u|",
                                   pin->kind, pin->location, pin->ref,
                                   pin->commit, pin->sha256, pin->revision);
    for (size_t index = 0U; index < pin->patch_count && used < out_size; ++index) {
        int written = snprintf(out + used, out_size - used, "%s|",
                                pin->patches[index]);
        if (written < 0 || (size_t)written >= out_size - used) break;
        used += (size_t)written;
    }
}

static int recipe_directory_has_source(const char *directory)
{
    static const char *const markers[] = {
        "Forge.toml", "CMakeLists.txt", "Makefile", "makefile", "GNUmakefile"
    };
    char path[FORGE_PATH_MAX];
    for (size_t index = 0U; index < sizeof(markers) / sizeof(markers[0]); ++index) {
        if (snprintf(path, sizeof(path), "%s/%s", directory, markers[index]) >= 0) {
            FILE *file = fopen(path, "rb");
            if (file != NULL) {
                (void)fclose(file);
                return 1;
            }
        }
    }
    return 0;
}

static void recipe_read_marker(const char *directory, char *out, size_t out_size)
{
    char path[FORGE_PATH_MAX];
    FILE *file;
    size_t length;
    out[0] = '\0';
    if (snprintf(path, sizeof(path), "%s/.forge-pin-sha256", directory) < 0) return;
    file = fopen(path, "rb");
    if (file == NULL) return;
    length = fread(out, 1U, out_size - 1U, file);
    (void)fclose(file);
    out[length] = '\0';
    out[strcspn(out, "\r\n")] = '\0';
}

static int recipe_write_marker(const char *directory, const char *value,
                               char *error, size_t error_size)
{
    char path[FORGE_PATH_MAX];
    FILE *file;
    if (snprintf(path, sizeof(path), "%s/.forge-pin-sha256", directory) < 0) {
        forge_util_set_error(error, error_size, "registry cache path is too long");
        return -1;
    }
    file = fopen(path, "wb");
    if (file == NULL || fputs(value, file) < 0 || fputc('\n', file) == EOF ||
        fclose(file) != 0) {
        if (file != NULL) (void)fclose(file);
        forge_util_set_error(error, error_size, "cannot record registry source pin");
        return -1;
    }
    return 0;
}

static int package_name_is_portable(const char *name);

static void registry_cache_hash(const char *text, char *hex, size_t hex_size)
{
    unsigned long long hash = 1469598103934665603ULL;
    for (const unsigned char *cursor = (const unsigned char *)text;
         *cursor != 0U; ++cursor) {
        hash ^= *cursor;
        hash *= 1099511628211ULL;
    }
    (void)snprintf(hex, hex_size, "%016llx", hash);
}

static int apply_registry_patches(ForgeLogger *logger, const char *base,
                                  const ForgeRegistryPin *pin,
                                  const char *root, const char *patch_dir,
                                  char *error, size_t error_size)
{
    for (size_t index = 0U; index < pin->patch_count; ++index) {
        char url[FORGE_PATH_MAX];
        char patch_path[FORGE_PATH_MAX];
        ForgeArgv argv = { 0 };
        int exit_code = 0;

        if (snprintf(url, sizeof(url), "%s/patches/%s", base,
                     pin->patches[index]) < 0 ||
            strlen(base) + strlen(pin->patches[index]) + 9U >= sizeof(url) ||
            snprintf(patch_path, sizeof(patch_path), "%s/%s", patch_dir,
                     pin->patches[index]) < 0 ||
            strlen(patch_dir) + strlen(pin->patches[index]) + 1U >=
                sizeof(patch_path)) {
            forge_util_set_error(error, error_size, "registry patch path is too long");
            return -1;
        }
        if (forge_fetch_url_is_supported(url, error, error_size) != 0 ||
            forge_fetch_to_file(logger, url, patch_path, error, error_size) != 0) {
            return -1;
        }
        if (forge_argv_append(&argv, "git") != 0 ||
            forge_argv_append(&argv, "-C") != 0 ||
            forge_argv_append(&argv, root) != 0 ||
            forge_argv_append(&argv, "apply") != 0 ||
            forge_argv_append(&argv, "--check") != 0 ||
            forge_argv_append(&argv, "--") != 0 ||
            forge_argv_append(&argv, patch_path) != 0 ||
            forge_argv_finalize(&argv) != 0 ||
            forge_process_run(argv.items, NULL, 0, &exit_code,
                              error, error_size) != 0 || exit_code != 0) {
            forge_argv_free(&argv);
            forge_util_set_error(error, error_size,
                      "registry patch '%s' does not apply to '%s'",
                      pin->patches[index], root);
            return -1;
        }
        forge_argv_free(&argv);
        {
            ForgeArgv apply = { 0 };
            if (forge_argv_append(&apply, "git") != 0 ||
                forge_argv_append(&apply, "-C") != 0 ||
                forge_argv_append(&apply, root) != 0 ||
                forge_argv_append(&apply, "apply") != 0 ||
                forge_argv_append(&apply, "--") != 0 ||
                forge_argv_append(&apply, patch_path) != 0 ||
                forge_argv_finalize(&apply) != 0 ||
                forge_process_run(apply.items, NULL, 0, &exit_code,
                                  error, error_size) != 0 || exit_code != 0) {
                forge_argv_free(&apply);
                forge_util_set_error(error, error_size,
                          "could not apply registry patch '%s'",
                          pin->patches[index]);
                return -1;
            }
            forge_argv_free(&apply);
        }
    }
    return 0;
}

/*
 * Cache directory discriminant for the declared feature request. A bare
 * request with defaults on (the common, pre-feature layout) takes no
 * suffix, preserving existing cache directories; otherwise "+" plus the
 * canonical declared names, with "~nodefault" when defaults are off.
 * Names are manifest-validated, so "+", "," and "~" are the only
 * punctuation and all are path-safe on every host. The effective set
 * (defaults resolved) lives in the lock pin instead: the same spelling
 * always builds the same bytes per recipe, and recipe default changes
 * move the pin rather than the directory.
 */
static int features_dir_suffix(const char *declared, int use_defaults,
                               char *out, size_t out_size)
{
    if ((declared == NULL || declared[0] == '\0') && use_defaults) {
        out[0] = '\0';
        return 0;
    }
    if ((size_t)snprintf(out, out_size, "+%s%s",
                         declared != NULL ? declared : "",
                         use_defaults ? "" : "~nodefault") >= out_size) {
        return -1;
    }
    return 0;
}

int forge_registry_materialize(ForgeLogger *logger, const char *dep_name,
                               const char *package,
                               const char *wanted_version,
                               const char *min_version,
                               const char *range,
                               const char *max_version,
                               const char *declared_features,
                               int use_defaults,
                               const char *lock_version, const char *lock_kind,
                               const char *lock_location, const char *lock_ref,
                               const char *lock_commit, const char *lock_sha256,
                               unsigned lock_revision,
                               const char *lock_features,
                               int force_update, int offline,
                               const char *package_dir,
                               char *root_out, size_t root_size,
                               ForgeRegistryPin *pin, ForgeFeatureDefs *defs,
                               int *reused,
                               char *error, size_t error_size)
{
    char base[FORGE_PATH_MAX];
    char version[FORGE_MANIFEST_VALUE_MAX];
    char floor_version[FORGE_MANIFEST_VALUE_MAX];
    unsigned floor_revision = 0U;
    /* Declared-spelling suffix buffer: 16 manifest names worst case, well
     * above the effective set the lock records. */
    char feature_suffix[1024];
    char safe[FORGE_PATH_MAX];
    char version_dir[FORGE_PATH_MAX];
    char resolve_tmp[FORGE_PATH_MAX];
    char marker[FORGE_PATH_MAX * 2U];
    char identity[FORGE_PATH_MAX * 2U];
    int query_latest;
    int min_given = min_version != NULL && min_version[0] != '\0';
    int range_given = range != NULL && range[0] != '\0';
    int max_given = max_version != NULL && max_version[0] != '\0';

    if (pin == NULL || reused == NULL || defs == NULL) {
        forge_util_set_error(error, error_size, "registry materialize needs a pin, feature definitions, and reuse flag");
        return -1;
    }
    memset(pin, 0, sizeof(*pin));
    *reused = 0;
    if (forge_registry_base_url(base, sizeof(base), error, error_size) != 0 ||
        !package_name_is_portable(package)) {
        forge_util_set_error(error, error_size, "invalid registry package name '%s'",
                             package != NULL ? package : "<null>");
        return -1;
    }
    if (features_dir_suffix(declared_features, use_defaults,
                            feature_suffix, sizeof(feature_suffix)) != 0) {
        forge_util_set_error(error, error_size, "registry feature set is too long");
        return -1;
    }
    query_latest = 0;
    floor_version[0] = '\0';
    if (range_given && !forge_version_range_is_valid(range)) {
        forge_util_set_error(error, error_size,
                  "dependency '%s': version-range '%s' is not valid",
                  dep_name != NULL ? dep_name : package, range);
        return -1;
    }
    if (max_given && !version_text_is_valid(max_version)) {
        forge_util_set_error(error, error_size,
                  "dependency '%s': max-version '%s' is not a valid version",
                  dep_name != NULL ? dep_name : package, max_version);
        return -1;
    }
    if (wanted_version != NULL && wanted_version[0] != '\0') {
        /* Exact pins name their bytes outright and bypass the baseline. */
        (void)snprintf(version, sizeof(version), "%s", wanted_version);
    } else if ((range_given || max_given) && !force_update &&
               lock_version != NULL && lock_version[0] != '\0') {
        /*
         * Locked range/max reuse: a pin that already satisfies every
         * constraint stays put without touching the network. The
         * baseline still floors fresh resolutions below; it never
         * ambushes a locked build, so only enforce it when the lock
         * predates the floor check via resolve_floor when needed.
         */
        int lock_ok = 1;

        if (range_given && !forge_version_satisfies(lock_version, range)) {
            lock_ok = 0;
        }
        if (lock_ok && max_given &&
            forge_version_compare(lock_version, max_version) > 0) {
            lock_ok = 0;
        }
        if (lock_ok && min_given &&
            forge_version_compare(lock_version, min_version) < 0) {
            lock_ok = 0;
        }
        if (lock_ok) {
            (void)snprintf(version, sizeof(version), "%s", lock_version);
        } else {
            version[0] = '\0';
            query_latest = 2; /* range/max re-pick below */
        }
    } else if ((range_given || max_given) && force_update) {
        version[0] = '\0';
        query_latest = 2; /* range/max re-pick below */
    } else if ((range_given || max_given)) {
        version[0] = '\0';
        query_latest = 2; /* fresh range/max re-pick below */
    } else if (force_update && !min_given) {
        /*
         * Bare update: track newest. The baseline floors fresh
         * resolutions; it never holds back an explicit update.
         */
        version[0] = '\0';
        query_latest = 1;
    } else if (!force_update && lock_version != NULL && lock_version[0] != '\0' &&
               (!min_given ||
                forge_version_compare(lock_version, min_version) >= 0)) {
        /*
         * A lock pin that already satisfies the minimum stays put without
         * touching the network: the baseline governs fresh and update
         * resolutions, it never ambushes a locked build.
         */
        (void)snprintf(version, sizeof(version), "%s", lock_version);
    } else {
        /*
         * Fresh bare/minimum resolution, a lock below the new minimum, or
         * an update carrying a minimum: the floor decides. With no floor
         * at all a bare entry tracks newest; updates always track newest
         * and check the floor after the query.
         */
        if (resolve_floor(logger, dep_name, base, package,
                          min_given ? min_version : "",
                          resolve_tmp, offline, floor_version,
                          sizeof(floor_version), &floor_revision,
                          error, error_size) != 0) {
            return -1;
        }
        if (force_update || floor_version[0] == '\0') {
            version[0] = '\0';
            query_latest = 1;
        } else {
            (void)snprintf(version, sizeof(version), "%s", floor_version);
        }
    }
    if (query_latest == 2) {
        /*
         * Range/max resolution: list every indexed release newest-first
         * and take the first entry satisfying the range, the manifest
         * minimum, the baseline floor, and the inclusive maximum.
         * Exact pins never reach here; bare/minimum entries never set
         * query_latest=2 above.
         */
        static char listed[FORGE_REGISTRY_MAX_LISTED_VERSIONS][FORGE_MANIFEST_VALUE_MAX];
        static unsigned listed_rev[FORGE_REGISTRY_MAX_LISTED_VERSIONS];
        size_t nlisted = 0U;
        size_t i;
        int picked = 0;

        (void)snprintf(resolve_tmp, sizeof(resolve_tmp), "%s/.resolve.json.tmp",
                       package_dir);
        if (resolve_floor(logger, dep_name, base, package,
                          min_given ? min_version : "", resolve_tmp, offline,
                          floor_version, sizeof(floor_version),
                          &floor_revision, error, error_size) != 0) {
            return -1;
        }
        if (offline && (lock_version == NULL || lock_version[0] == '\0')) {
            forge_util_set_error(error, error_size,
                      "dependency '%s' is not cached and --offline forbids fetching it",
                      dep_name != NULL ? dep_name : package);
            return -1;
        }
        if (registry_list_versions(logger, base, package, resolve_tmp,
                                   listed, listed_rev,
                                   FORGE_REGISTRY_MAX_LISTED_VERSIONS,
                                   &nlisted, error, error_size) != 0) {
            return -1;
        }
        sort_versions_desc(listed, listed_rev, nlisted);
        for (i = 0U; i < nlisted; ++i) {
            if (range_given && !forge_version_satisfies(listed[i], range)) {
                continue;
            }
            if (min_given &&
                forge_version_compare(listed[i], min_version) < 0) {
                continue;
            }
            if (max_given &&
                forge_version_compare(listed[i], max_version) > 0) {
                continue;
            }
            if (floor_version[0] != '\0') {
                int order = forge_version_compare(listed[i], floor_version);

                if (order < 0) {
                    continue;
                }
            }
            (void)snprintf(version, sizeof(version), "%s", listed[i]);
            picked = 1;
            break;
        }
        if (!picked) {
            forge_util_set_error(error, error_size,
                      "dependency '%s': no registry release satisfies '%s%s%s%s'",
                      dep_name != NULL ? dep_name : package,
                      range_given ? range : "",
                      (range_given && max_given) ? ", " : "",
                      max_given ? "<=" : "",
                      max_given ? max_version : "");
            return -1;
        }
        query_latest = 0;
    }
    if (version[0] != '\0') {
        size_t suffix_length = strlen(feature_suffix);

        forge_paths_safe_output_name(version, safe, sizeof(safe));
        if (lock_kind != NULL && strcmp(lock_kind, "git") == 0 &&
            lock_location != NULL && lock_location[0] != '\0') {
            char cache_hash[32];
            registry_cache_hash(lock_location, cache_hash, sizeof(cache_hash));
            if (strlen(package_dir) + strlen(safe) + strlen(cache_hash) + 2U +
                suffix_length >= sizeof(version_dir)) {
                forge_util_set_error(error, error_size, "registry cache path is too long");
                return -1;
            }
            {
                size_t base_length = strlen(package_dir);
                size_t name_length = strlen(safe);
                size_t hash_length = strlen(cache_hash);
                memcpy(version_dir, package_dir, base_length);
                version_dir[base_length] = '/';
                memcpy(version_dir + base_length + 1U, safe, name_length);
                version_dir[base_length + 1U + name_length] = '-';
                memcpy(version_dir + base_length + 2U + name_length,
                       cache_hash, hash_length);
                memcpy(version_dir + base_length + 2U + name_length + hash_length,
                       feature_suffix, suffix_length + 1U);
            }
        } else {
        if (strlen(package_dir) + strlen(safe) + 1U + suffix_length >= sizeof(version_dir)) {
            forge_util_set_error(error, error_size, "registry cache path is too long");
            return -1;
        }
        {
            size_t base_length = strlen(package_dir);
            size_t name_length = strlen(safe);
            memcpy(version_dir, package_dir, base_length);
            version_dir[base_length] = '/';
            memcpy(version_dir + base_length + 1U, safe, name_length);
            memcpy(version_dir + base_length + 1U + name_length,
                   feature_suffix, suffix_length + 1U);
        }
        }
    } else {
        version_dir[0] = '\0';
    }
    (void)snprintf(resolve_tmp, sizeof(resolve_tmp), "%s/.resolve.json.tmp",
                   package_dir);

    if (version_dir[0] != '\0' && lock_kind != NULL && lock_kind[0] != '\0') {
        ForgeRegistryPin locked = { 0 };
        (void)snprintf(locked.version, sizeof(locked.version), "%s", lock_version);
        (void)snprintf(locked.kind, sizeof(locked.kind), "%s", lock_kind);
        (void)snprintf(locked.location, sizeof(locked.location), "%s", lock_location);
        (void)snprintf(locked.ref, sizeof(locked.ref), "%s", lock_ref);
        (void)snprintf(locked.commit, sizeof(locked.commit), "%s", lock_commit);
        (void)snprintf(locked.sha256, sizeof(locked.sha256), "%s", lock_sha256);
        locked.revision = lock_revision;
        registry_identity(&locked, identity, sizeof(identity));
        recipe_read_marker(version_dir, marker, sizeof(marker));
        if (!force_update && strcmp(identity, marker) == 0 &&
            recipe_directory_has_source(version_dir)) {
            /*
             * Cache hit: the bytes stay, but downstream still needs the
             * recipe's feature definitions (effective-set validation, cflag
             * folding, feature dependencies). Re-read the exact locked
             * recipe unless definitions cannot matter (no declared request
             * and an empty recorded set) or cannot be reached (remote
             * registry while --offline). A skipped refresh leaves defs
             * empty and the caller falls back to the lock's recorded set.
             */
            int need_defs = (declared_features != NULL && declared_features[0] != '\0') ||
                (lock_features != NULL && lock_features[0] != '\0');
            int recipe_local = strncmp(base, "file://", 7U) == 0;

            if (need_defs && lock_version != NULL && lock_version[0] != '\0' &&
                (!offline || recipe_local)) {
                if (forge_registry_query(logger, package, lock_version,
                                         resolve_tmp, pin, defs, error,
                                         error_size) != 0) {
                    return -1;
                }
            } else {
                (void)snprintf(pin->version, sizeof(pin->version), "%s", locked.version);
                (void)snprintf(pin->kind, sizeof(pin->kind), "%s", locked.kind);
                (void)snprintf(pin->location, sizeof(pin->location), "%s", locked.location);
                (void)snprintf(pin->ref, sizeof(pin->ref), "%s", locked.ref);
                (void)snprintf(pin->commit, sizeof(pin->commit), "%s", locked.commit);
                (void)snprintf(pin->sha256, sizeof(pin->sha256), "%s", locked.sha256);
                pin->revision = locked.revision;
            }
            (void)snprintf(root_out, root_size, "%s", version_dir);
            *reused = 1;
            return 0;
        }
    }
    if (offline) {
        forge_util_set_error(error, error_size,
                  "dependency '%s' is not cached and --offline forbids fetching it",
                  dep_name != NULL ? dep_name : package);
        return -1;
    }
    if (forge_registry_query(logger, package, version, resolve_tmp, pin, defs,
                             error, error_size) != 0) return -1;
    if (range_given && !forge_version_satisfies(pin->version, range)) {
        forge_util_set_error(error, error_size,
                  "dependency '%s': registry release %s does not satisfy "
                  "version-range '%s'",
                  dep_name != NULL ? dep_name : package, pin->version,
                  range);
        return -1;
    }
    if (max_given && forge_version_compare(pin->version, max_version) > 0) {
        forge_util_set_error(error, error_size,
                  "dependency '%s': registry release %s exceeds max-version %s",
                  dep_name != NULL ? dep_name : package, pin->version,
                  max_version);
        return -1;
    }
    if (query_latest && floor_version[0] != '\0' &&
        (forge_version_compare(pin->version, floor_version) < 0 ||
         (forge_version_compare(pin->version, floor_version) == 0 &&
          pin->revision < floor_revision))) {
        forge_util_set_error(error, error_size,
                  "dependency '%s': newest registry release %s is below the "
                  "required minimum %s",
                  dep_name != NULL ? dep_name : package, pin->version,
                  floor_version);
        return -1;
    }
    if (!force_update && lock_version != NULL && lock_version[0] != '\0' &&
        strcmp(pin->version, lock_version) == 0 &&
        ((lock_kind != NULL && strcmp(pin->kind, lock_kind) != 0) ||
         (lock_location != NULL && strcmp(pin->location, lock_location) != 0) ||
         (lock_ref != NULL && strcmp(pin->ref, lock_ref) != 0) ||
         (lock_sha256 != NULL && strcmp(pin->sha256, lock_sha256) != 0))) {
        forge_util_set_error(error, error_size,
                  "dependency '%s': registry recipe changed under Forge.lock; "
                  "run 'forge update' to regenerate it", dep_name);
        return -1;
    }
    forge_paths_safe_output_name(pin->version, safe, sizeof(safe));
    if (strcmp(pin->kind, "git") != 0) {
        if (strlen(package_dir) + strlen(safe) + 1U >= sizeof(version_dir)) {
            forge_util_set_error(error, error_size, "registry cache path is too long");
            return -1;
        }
        {
            size_t base_length = strlen(package_dir);
            size_t name_length = strlen(safe);
            memcpy(version_dir, package_dir, base_length);
            version_dir[base_length] = '/';
            memcpy(version_dir + base_length + 1U, safe, name_length + 1U);
        }
    }
    registry_identity(pin, identity, sizeof(identity));
    recipe_read_marker(version_dir, marker, sizeof(marker));
    if (!force_update && strcmp(identity, marker) == 0 &&
        recipe_directory_has_source(version_dir)) {
        (void)snprintf(root_out, root_size, "%s", version_dir);
        *reused = 1;
        return 0;
    }
    forge_paths_remove_tree(version_dir, NULL, 0U);
    if (strcmp(pin->kind, "git") == 0) {
        char resolved[FORGE_MANIFEST_VALUE_MAX];
        char cache_hash[32];
        (void)registry_cache_hash(pin->location, cache_hash, sizeof(cache_hash));
        if (strlen(package_dir) + strlen(safe) + strlen(cache_hash) + 2U >=
            sizeof(version_dir)) {
            forge_util_set_error(error, error_size, "registry cache path is too long");
            return -1;
        }
        {
            size_t base_length = strlen(package_dir);
            size_t name_length = strlen(safe);
            size_t hash_length = strlen(cache_hash);
            memcpy(version_dir, package_dir, base_length);
            version_dir[base_length] = '/';
            memcpy(version_dir + base_length + 1U, safe, name_length);
            version_dir[base_length + 1U + name_length] = '-';
            memcpy(version_dir + base_length + 2U + name_length,
                   cache_hash, hash_length + 1U);
        }
        forge_paths_remove_tree(version_dir, NULL, 0U);
        if (forge_paths_ensure_directory(package_dir, error, error_size) != 0 ||
            forge_deps_ensure_git_checkout(logger, dep_name, pin->location,
                pin->ref, lock_kind != NULL && strcmp(lock_kind, "git") == 0 ?
                lock_commit : "", force_update, 0, offline, version_dir,
                resolved, sizeof(resolved), error, error_size) != 0) return -1;
        (void)snprintf(pin->commit, sizeof(pin->commit), "%s", resolved);
    } else {
        char archive[FORGE_PATH_MAX];
        char actual[FORGE_SHA256_HEX_LENGTH + 1U];
        if (strlen(package_dir) + strlen(safe) + 11U >= sizeof(archive)) {
            forge_util_set_error(error, error_size, "registry cache path is too long");
            return -1;
        }
        {
            size_t base_length = strlen(package_dir);
            size_t name_length = strlen(safe);
            memcpy(archive, package_dir, base_length);
            archive[base_length] = '/';
            archive[base_length + 1U] = '.';
            memcpy(archive + base_length + 2U, safe, name_length);
            memcpy(archive + base_length + 2U + name_length,
                   ".tgz.tmp", 9U);
        }
        if (forge_fetch_to_file(logger, pin->location, archive, error, error_size) != 0 ||
            forge_sha256_file(archive, actual, error, error_size) != 0) {
            (void)remove(archive);
            return -1;
        }
        if (strcmp(actual, pin->sha256) != 0) {
            forge_util_set_error(error, error_size,
                      "dependency '%s': source sha256 does not match recipe",
                      package);
            (void)remove(archive);
            return -1;
        }
        if (forge_fetch_unpack_tar_gz(logger, archive, version_dir, error,
                                      error_size) != 0) {
            (void)remove(archive);
            return -1;
        }
        (void)remove(archive);
    }
    {
        char patch_dir[FORGE_PATH_MAX];
        if (snprintf(patch_dir, sizeof(patch_dir), "%s/.patches", version_dir) < 0 ||
            forge_paths_ensure_directory(patch_dir, error, error_size) != 0 ||
            apply_registry_patches(logger, base, pin, version_dir, patch_dir,
                                   error, error_size) != 0) return -1;
    }
    if (!recipe_directory_has_source(version_dir)) {
        forge_util_set_error(error, error_size,
                  "dependency '%s' has no buildable source after registry patches",
                  package);
        return -1;
    }
    registry_identity(pin, identity, sizeof(identity));
    if (recipe_write_marker(version_dir, identity, error, error_size) != 0 ||
        (size_t)snprintf(root_out, root_size, "%s", version_dir) >= root_size) {
        forge_util_set_error(error, error_size, "registry cache path is too long");
        return -1;
    }
    return 0;
}

/* ------------------------------------------------------------------ */
/* Manifest value validation                                           */
/* ------------------------------------------------------------------ */

/* Registry package names travel in URLs and cache paths: keep them to
 * the same portable alphabet dependency names already use. */
static int package_name_is_portable(const char *name)
{
    size_t index;

    if (name == NULL || name[0] == '\0') {
        return 0;
    }
    for (index = 0U; name[index] != '\0'; ++index) {
        unsigned char character = (unsigned char)name[index];

        if (!(isalnum(character) || character == '-' || character == '_' ||
              character == '.')) {
            return 0;
        }
    }
    return 1;
}

/* Versions travel in URLs, paths, and the lockfile: digits, letters,
 * and the usual prerelease punctuation only. */
static int version_text_is_valid(const char *version)
{
    size_t index;

    if (version == NULL || version[0] == '\0') {
        return 0;
    }
    for (index = 0U; version[index] != '\0'; ++index) {
        unsigned char character = (unsigned char)version[index];

        if (!(isalnum(character) || character == '.' || character == '-' ||
              character == '_' || character == '+')) {
            return 0;
        }
    }
    return 1;
}

static int sha256_text_is_valid(const char *text)
{
    size_t index;

    if (text == NULL || strlen(text) != FORGE_SHA256_HEX_LENGTH) {
        return 0;
    }
    for (index = 0U; index < FORGE_SHA256_HEX_LENGTH; ++index) {
        unsigned char character = (unsigned char)text[index];

        if (!((character >= '0' && character <= '9') ||
              (character >= 'a' && character <= 'f'))) {
            return 0;
        }
    }
    return 1;
}

/* ------------------------------------------------------------------ */
/* Minimal strict JSON reader                                          */
/*                                                                     */
/* The resolve endpoint answers with a tiny known shape; a strict      */
/* hand reader (known keys, loud errors) beats vendoring a JSON        */
/* library forge would then carry forever. Anything unexpected —       */
/* truncation, wrong types, \u escapes — fails instead of guessing.    */
/* ------------------------------------------------------------------ */

/* (ForgeJsonCursor forward-declared at the top of this file.) */

static void json_skip_space(ForgeJsonCursor *cursor)
{
    while (isspace((unsigned char)*cursor->text)) {
        ++cursor->text;
    }
}

/* Copies a JSON string starting at the opening quote into `out`
 * (unescaping \" \\ \/ \b \f \n \r \t). Returns 0 on success. */
static int json_read_string(ForgeJsonCursor *cursor, char *out,
                            size_t out_size)
{
    size_t length = 0U;

    if (*cursor->text != '"') {
        cursor->error = "expected a string";
        return -1;
    }
    ++cursor->text;
    for (;;) {
        char character = *cursor->text;

        if (character == '\0') {
            cursor->error = "truncated string";
            return -1;
        }
        if (character == '"') {
            ++cursor->text;
            break;
        }
        if (character == '\\') {
            ++cursor->text;
            character = *cursor->text;
            if (character == '\0') {
                cursor->error = "truncated escape";
                return -1;
            }
            switch (character) {
            case '"': character = '"'; break;
            case '\\': character = '\\'; break;
            case '/': character = '/'; break;
            case 'b': character = '\b'; break;
            case 'f': character = '\f'; break;
            case 'n': character = '\n'; break;
            case 'r': character = '\r'; break;
            case 't': character = '\t'; break;
            default:
                cursor->error = "unsupported escape (only simple escapes; "
                                "no \\u)";
                return -1;
            }
            ++cursor->text;
        } else {
            ++cursor->text;
        }
        if (length + 1U >= out_size) {
            cursor->error = "string does not fit";
            return -1;
        }
        out[length++] = character;
    }
    out[length] = '\0';
    return 0;
}

/* Advances past one JSON string (opening quote at cursor) without
 * storing it. Returns 0 on success. */
static int json_skip_string(ForgeJsonCursor *cursor)
{
    if (*cursor->text != '"') {
        cursor->error = "expected a string";
        return -1;
    }
    ++cursor->text;
    for (;;) {
        char character = *cursor->text;

        if (character == '\0') {
            cursor->error = "truncated string";
            return -1;
        }
        if (character == '"') {
            ++cursor->text;
            return 0;
        }
        if (character == '\\') {
            ++cursor->text;
            if (*cursor->text == '\0') {
                cursor->error = "truncated escape";
                return -1;
            }
            ++cursor->text;
            continue;
        }
        ++cursor->text;
    }
}

/* Skips one JSON value (object/array/string/number/literal), string-aware. */
static int json_skip_value(ForgeJsonCursor *cursor)
{
    char opener;

    json_skip_space(cursor);
    opener = *cursor->text;
    if (opener == '"') {
        return json_skip_string(cursor);
    }
    if (opener == '{' || opener == '[') {
        char closer = opener == '{' ? '}' : ']';
        int depth = 0;

        do {
            if (*cursor->text == '\0') {
                cursor->error = "truncated value";
                return -1;
            }
            if (*cursor->text == '"') {
                if (json_skip_string(cursor) != 0) {
                    return -1;
                }
                continue;
            }
            if (*cursor->text == opener) {
                ++depth;
            } else if (*cursor->text == closer) {
                --depth;
            }
            ++cursor->text;
        } while (depth > 0);
        return 0;
    }
    /* number or literal: consume to the next structural character. */
    while (*cursor->text != '\0' && *cursor->text != ',' &&
           *cursor->text != '}' && *cursor->text != ']' &&
           !isspace((unsigned char)*cursor->text)) {
        ++cursor->text;
    }
    return 0;
}

/*
 * Walks one object level: when `wanted` matches a key, reads its string
 * value into `out` (or reports null via `is_null`); every other value is
 * skipped. Returns 1 when the key was found (string or null), 0 when
 * absent, -1 on malformed input. `cursor` must sit at '{'.
 */
static int json_object_string(ForgeJsonCursor *cursor, const char *wanted,
                              char *out, size_t out_size, int *is_null)
{
    int first = 1;

    *is_null = 0;
    json_skip_space(cursor);
    if (*cursor->text != '{') {
        cursor->error = "expected an object";
        return -1;
    }
    ++cursor->text;
    for (;;) {
        char key[128];

        json_skip_space(cursor);
        if (*cursor->text == '}') {
            ++cursor->text;
            return 0;
        }
        if (!first) {
            if (*cursor->text != ',') {
                cursor->error = "expected ',' or '}'";
                return -1;
            }
            ++cursor->text;
            json_skip_space(cursor);
        }
        first = 0;
        if (json_read_string(cursor, key, sizeof(key)) != 0) {
            return -1;
        }
        json_skip_space(cursor);
        if (*cursor->text != ':') {
            cursor->error = "expected ':'";
            return -1;
        }
        ++cursor->text;
        json_skip_space(cursor);
        if (strcmp(key, wanted) == 0) {
            if (strncmp(cursor->text, "null", 4U) == 0) {
                *is_null = 1;
                cursor->text += 4U;
                return 1;
            }
            if (json_read_string(cursor, out, out_size) != 0) {
                return -1;
            }
            return 1;
        }
        if (json_skip_value(cursor) != 0) {
            return -1;
        }
    }
}

/*
 * Same walk as json_object_string but for a non-negative integer value
 * (recipe and baseline revisions). Rejects signs, fractions, exponents,
 * and values above `max`; null counts as found-with-null. Returns
 * 1 when the key was found, 0 when absent, -1 on malformed input.
 */
static int json_object_uint(ForgeJsonCursor *cursor, const char *wanted,
                            unsigned *out, unsigned max, int *is_null)
{
    int first = 1;

    *is_null = 0;
    json_skip_space(cursor);
    if (*cursor->text != '{') {
        cursor->error = "expected an object";
        return -1;
    }
    ++cursor->text;
    for (;;) {
        char key[128];

        json_skip_space(cursor);
        if (*cursor->text == '}') {
            ++cursor->text;
            return 0;
        }
        if (!first) {
            if (*cursor->text != ',') {
                cursor->error = "expected ',' or '}'";
                return -1;
            }
            ++cursor->text;
            json_skip_space(cursor);
        }
        first = 0;
        if (json_read_string(cursor, key, sizeof(key)) != 0) {
            return -1;
        }
        json_skip_space(cursor);
        if (*cursor->text != ':') {
            cursor->error = "expected ':'";
            return -1;
        }
        ++cursor->text;
        json_skip_space(cursor);
        if (strcmp(key, wanted) == 0) {
            unsigned long value = 0UL;

            if (strncmp(cursor->text, "null", 4U) == 0) {
                *is_null = 1;
                cursor->text += 4U;
                return 1;
            }
            if (!isdigit((unsigned char)*cursor->text)) {
                cursor->error = "expected an integer";
                return -1;
            }
            while (isdigit((unsigned char)*cursor->text)) {
                value = value * 10UL + (unsigned long)(*cursor->text - '0');
                if (value > max) {
                    cursor->error = "integer is too large";
                    return -1;
                }
                ++cursor->text;
            }
            *out = (unsigned)value;
            return 1;
        }
        if (json_skip_value(cursor) != 0) {
            return -1;
        }
    }
}

/*
 * Reads `outer.inner` (one nesting level, e.g. artifact.url). Follows the
 * same found/absent/malformed contract; null counts as found-with-null.
 */static int json_nested_string(const char *json, const char *outer,
                              const char *inner, char *out, size_t out_size,
                              int *is_null)
{
    ForgeJsonCursor cursor = { json, NULL };
    const char *saved;
    int found;
    int inner_null = 0;

    *is_null = 0;
    json_skip_space(&cursor);
    if (*cursor.text != '{') {
        return -1;
    }
    /* Walk the top level without consuming: find the outer object span. */
    {
        int first = 1;

        ++cursor.text; /* consume '{' */
        for (;;) {
            char key[128];

            json_skip_space(&cursor);
            if (*cursor.text == '}') {
                return 0;
            }
            if (!first) {
                if (*cursor.text != ',') {
                    return -1;
                }
                ++cursor.text;
                json_skip_space(&cursor);
            }
            first = 0;
            if (json_read_string(&cursor, key, sizeof(key)) != 0) {
                return -1;
            }
            json_skip_space(&cursor);
            if (*cursor.text != ':') {
                return -1;
            }
            ++cursor.text;
            json_skip_space(&cursor);
            if (strcmp(key, outer) != 0) {
                if (json_skip_value(&cursor) != 0) {
                    return -1;
                }
                continue;
            }
            if (strncmp(cursor.text, "null", 4U) == 0) {
                *is_null = 1;
                return 1;
            }
            saved = cursor.text;
            break;
        }
    }
    {
        ForgeJsonCursor inner_cursor = { saved, NULL };

        found = json_object_string(&inner_cursor, inner, out, out_size,
                                   &inner_null);
        if (found < 0) {
            return -1;
        }
        *is_null = inner_null;
        return found;
    }
}

static int json_array_strings(const char *json, const char *wanted,
                              char values[][FORGE_PATH_MAX], size_t capacity,
                              size_t *count)
{
    ForgeJsonCursor cursor = { json, NULL };
    int first = 1;

    *count = 0U;
    json_skip_space(&cursor);
    if (*cursor.text != '{') return -1;
    ++cursor.text;
    for (;;) {
        char key[128];
        json_skip_space(&cursor);
        if (*cursor.text == '}') return 0;
        if (!first) {
            if (*cursor.text != ',') return -1;
            ++cursor.text;
            json_skip_space(&cursor);
        }
        first = 0;
        if (json_read_string(&cursor, key, sizeof(key)) != 0) return -1;
        json_skip_space(&cursor);
        if (*cursor.text != ':') return -1;
        ++cursor.text;
        json_skip_space(&cursor);
        if (strcmp(key, wanted) != 0) {
            if (json_skip_value(&cursor) != 0) return -1;
            continue;
        }
        if (*cursor.text != '[') return -1;
        ++cursor.text;
        for (;;) {
            json_skip_space(&cursor);
            if (*cursor.text == ']') return 0;
            if (*count == capacity || json_read_string(&cursor,
                    values[*count], FORGE_PATH_MAX) != 0) return -1;
            ++*count;
            json_skip_space(&cursor);
            if (*cursor.text == ',') {
                ++cursor.text;
                continue;
            }
            if (*cursor.text == ']') return 0;
            return -1;
        }
    }
}

static int parse_recipe(const char *body, const char *base,
                        ForgeRegistryPin *pin, char *error, size_t error_size)
{
    char value[FORGE_PATH_MAX];
    char absolute[FORGE_PATH_MAX];
    int is_null = 0;
    int found;

    found = json_object_string(&(ForgeJsonCursor){ body, NULL }, "version",
                               pin->version, sizeof(pin->version), &is_null);
    if (found <= 0 || is_null || !version_text_is_valid(pin->version)) {
        forge_util_set_error(error, error_size, "registry response has no usable version");
        return -1;
    }
    /* Recipe revisions share the registry schema's bound; older recipes
     * without the field mean revision 0. */
    pin->revision = 0U;
    found = json_object_uint(&(ForgeJsonCursor){ body, NULL }, "revision",
                             &pin->revision, FORGE_REGISTRY_MAX_REVISION,
                             &is_null);
    if (found < 0) {
        forge_util_set_error(error, error_size, "registry response has no usable revision");
        return -1;
    }
    found = json_nested_string(body, "source", "kind", pin->kind,
                               sizeof(pin->kind), &is_null);
    if (found <= 0 || is_null ||
        (strcmp(pin->kind, "git") != 0 && strcmp(pin->kind, "url") != 0)) {
        forge_util_set_error(error, error_size,
                  "registry response has no supported source kind");
        return -1;
    }
    found = json_nested_string(body, "source", "location", value,
                               sizeof(value), &is_null);
    if (found <= 0 || is_null || value[0] == '\0') {
        forge_util_set_error(error, error_size, "registry response has no source location");
        return -1;
    }
    if (value[0] == '/' && value[1] != '/') {
        if (snprintf(absolute, sizeof(absolute), "%s%s", base, value) < 0 ||
            strlen(base) + strlen(value) >= sizeof(absolute)) {
            forge_util_set_error(error, error_size, "registry source location is too long");
            return -1;
        }
    } else {
        if (snprintf(absolute, sizeof(absolute), "%s", value) < 0 ||
            strlen(value) >= sizeof(absolute)) {
            forge_util_set_error(error, error_size, "registry source location is too long");
            return -1;
        }
    }
    if (strcmp(pin->kind, "git") == 0) {
        found = json_nested_string(body, "source", "ref", pin->ref,
                                   sizeof(pin->ref), &is_null);
        if (found <= 0 || is_null || pin->ref[0] == '\0') {
            forge_util_set_error(error, error_size,
                      "registry Git source must have a supported location and ref");
            return -1;
        }
        if (forge_deps_git_url_is_supported(absolute, error, error_size) != 0) return -1;
    } else {
        found = json_nested_string(body, "source", "sha256", pin->sha256,
                                   sizeof(pin->sha256), &is_null);
        if (found <= 0 || is_null || !sha256_text_is_valid(pin->sha256)) {
            forge_util_set_error(error, error_size,
                      "registry URL source must have a supported location and sha256");
            return -1;
        }
        if (forge_fetch_url_is_supported(absolute, error, error_size) != 0) return -1;
    }
    (void)snprintf(pin->location, sizeof(pin->location), "%s", absolute);
    if (json_array_strings(body, "patches", pin->patches,
                           FORGE_REGISTRY_MAX_PATCHES, &pin->patch_count) < 0) {
        forge_util_set_error(error, error_size,
                  "registry response has an invalid patches array");
        return -1;
    }
    for (size_t index = 0U; index < pin->patch_count; ++index) {
        const char *name = pin->patches[index];
        if (name[0] == '\0' || strchr(name, '/') != NULL ||
            strchr(name, '\\') != NULL || strstr(name, "..") != NULL) {
            forge_util_set_error(error, error_size,
                      "registry response has an unsafe patch name");
            return -1;
        }
    }
    return 0;
}

/* ------------------------------------------------------------------ */
/* Recipe features                                                     */
/*                                                                     */
/* Optional named build variants: extra compiler flags plus extra      */
/* transitive (registry-only) dependencies. The consumer manifest only */
/* selects; definitions ride the recipe JSON the query already holds.  */
/* ------------------------------------------------------------------ */

static int parse_feature_dep(const char *span, ForgeFeatureDef *def,
                             char *error, size_t error_size)
{
    ForgeJsonCursor object;
    char package[FORGE_MANIFEST_VALUE_MAX];
    char version[FORGE_MANIFEST_VALUE_MAX] = { 0 };
    char min_version[FORGE_MANIFEST_VALUE_MAX] = { 0 };
    char range[FORGE_VERSION_RANGE_MAX] = { 0 };
    char max_version[FORGE_MANIFEST_VALUE_MAX] = { 0 };
    int is_null = 0;
    int found;
    ForgeFeatureDep *slot;

    if (def->dep_count == FORGE_FEATURE_DEPS_MAX) {
        forge_util_set_error(error, error_size,
                  "recipe feature '%s' lists more than %u dependencies",
                  def->name, (unsigned int)FORGE_FEATURE_DEPS_MAX);
        return -1;
    }
    object.text = span;
    object.error = NULL;
    found = json_object_string(&object, "registry", package,
                               sizeof(package), &is_null);
    if (found <= 0 || is_null || !package_name_is_portable(package)) {
        forge_util_set_error(error, error_size,
                  "recipe feature '%s' has a dependency without a usable "
                  "registry package", def->name);
        return -1;
    }
    object.text = span;
    object.error = NULL;
    found = json_object_string(&object, "version", version,
                               sizeof(version), &is_null);
    if (found < 0 || (found > 0 && !is_null &&
                      !version_text_is_valid(version))) {
        forge_util_set_error(error, error_size,
                  "recipe feature '%s' has a dependency without a usable "
                  "version", def->name);
        return -1;
    }
    object.text = span;
    object.error = NULL;
    found = json_object_string(&object, "min-version", min_version,
                               sizeof(min_version), &is_null);
    if (found < 0 || (found > 0 && !is_null &&
                      !version_text_is_valid(min_version))) {
        forge_util_set_error(error, error_size,
                  "recipe feature '%s' has a dependency without a usable "
                  "minimum version", def->name);
        return -1;
    }
    object.text = span;
    object.error = NULL;
    found = json_object_string(&object, "version-range", range,
                               sizeof(range), &is_null);
    if (found < 0 || (found > 0 && !is_null &&
                      !forge_version_range_is_valid(range))) {
        forge_util_set_error(error, error_size,
                  "recipe feature '%s' has a dependency without a usable "
                  "version-range", def->name);
        return -1;
    }
    object.text = span;
    object.error = NULL;
    found = json_object_string(&object, "max-version", max_version,
                               sizeof(max_version), &is_null);
    if (found < 0 || (found > 0 && !is_null &&
                      !version_text_is_valid(max_version))) {
        forge_util_set_error(error, error_size,
                  "recipe feature '%s' has a dependency without a usable "
                  "max-version", def->name);
        return -1;
    }
    {
        int pins = (version[0] != '\0') + (min_version[0] != '\0') +
                   (range[0] != '\0');

        if (pins > 1) {
            forge_util_set_error(error, error_size,
                      "recipe feature '%s' dependency '%s' pins more than "
                      "one of version, min-version, version-range",
                      def->name, package);
            return -1;
        }
        if (max_version[0] != '\0' && version[0] != '\0') {
            forge_util_set_error(error, error_size,
                      "recipe feature '%s' dependency '%s' combines "
                      "max-version with an exact version", def->name,
                      package);
            return -1;
        }
    }
    slot = &def->deps[def->dep_count++];
    (void)snprintf(slot->package, sizeof(slot->package), "%s", package);
    (void)snprintf(slot->version, sizeof(slot->version), "%s", version);
    (void)snprintf(slot->min_version, sizeof(slot->min_version), "%s",
                   min_version);
    (void)snprintf(slot->range, sizeof(slot->range), "%s", range);
    (void)snprintf(slot->max_version, sizeof(slot->max_version), "%s",
                   max_version);
    return 0;
}

static int parse_feature(const char *span, ForgeFeatureDefs *defs,
                         char *error, size_t error_size)
{
    ForgeJsonCursor object;
    ForgeFeatureDef *def;
    char flag_text[FORGE_FEATURE_CFLAG_MAX][FORGE_PATH_MAX];
    size_t flag_count = 0U;
    size_t index;
    int is_null = 0;
    int found;

    if (defs->count == FORGE_RECIPE_FEATURES_MAX) {
        forge_util_set_error(error, error_size,
                  "recipe defines more than %u features",
                  (unsigned int)FORGE_RECIPE_FEATURES_MAX);
        return -1;
    }
    def = &defs->items[defs->count];
    object.text = span;
    object.error = NULL;
    found = json_object_string(&object, "name", def->name,
                               sizeof(def->name), &is_null);
    if (found <= 0 || is_null || !forge_feature_name_is_valid(def->name)) {
        forge_util_set_error(error, error_size,
                  "recipe defines a feature without a usable name");
        return -1;
    }
    for (index = 0U; index < defs->count; ++index) {
        if (strcmp(defs->items[index].name, def->name) == 0) {
            forge_util_set_error(error, error_size,
                      "recipe defines feature '%s' twice", def->name);
            return -1;
        }
    }
    /* cflags: plain string array on the element span; over-capacity and
     * non-string elements fail inside the helper. */
    if (json_array_strings(span, "cflags", flag_text,
                           FORGE_FEATURE_CFLAG_MAX, &flag_count) != 0) {
        forge_util_set_error(error, error_size,
                  "recipe feature '%s' has an invalid cflags array",
                  def->name);
        return -1;
    }
    for (index = 0U; index < flag_count; ++index) {
        size_t flag_length = strlen(flag_text[index]);

        if (flag_length >= FORGE_MANIFEST_VALUE_MAX) {
            forge_util_set_error(error, error_size,
                      "recipe feature '%s' has an overlong cflag",
                      def->name);
            return -1;
        }
        memcpy(def->cflags[index], flag_text[index], flag_length + 1U);
    }
    def->cflag_count = flag_count;
    /* dependencies: optional array of registry-only {registry, version?,
     * min-version?} tables; unknown keys ride along per house rules. */
    {
        ForgeJsonCursor scan = { span, NULL };
        int first = 1;

        json_skip_space(&scan);
        if (*scan.text != '{') {
            forge_util_set_error(error, error_size,
                      "recipe feature '%s' is malformed", def->name);
            return -1;
        }
        ++scan.text;
        for (;;) {
            char key[128];

            json_skip_space(&scan);
            if (*scan.text == '}') {
                break;
            }
            if (!first) {
                if (*scan.text != ',') {
                    forge_util_set_error(error, error_size,
                              "recipe feature '%s' is malformed", def->name);
                    return -1;
                }
                ++scan.text;
                json_skip_space(&scan);
            }
            first = 0;
            if (json_read_string(&scan, key, sizeof(key)) != 0) {
                forge_util_set_error(error, error_size,
                          "recipe feature '%s' is malformed", def->name);
                return -1;
            }
            json_skip_space(&scan);
            if (*scan.text != ':') {
                forge_util_set_error(error, error_size,
                          "recipe feature '%s' is malformed", def->name);
                return -1;
            }
            ++scan.text;
            json_skip_space(&scan);
            if (strcmp(key, "dependencies") != 0) {
                if (json_skip_value(&scan) != 0) {
                    forge_util_set_error(error, error_size,
                              "recipe feature '%s' is malformed", def->name);
                    return -1;
                }
                continue;
            }
            if (*scan.text != '[') {
                forge_util_set_error(error, error_size,
                          "recipe feature '%s' has dependencies that are "
                          "not an array", def->name);
                return -1;
            }
            ++scan.text;
            for (;;) {
                const char *dep_start;
                char dep_span[2048];
                size_t dep_length;

                json_skip_space(&scan);
                if (*scan.text == ']') {
                    break;
                }
                if (*scan.text != '{') {
                    forge_util_set_error(error, error_size,
                              "recipe feature '%s' has a malformed "
                              "dependency", def->name);
                    return -1;
                }
                dep_start = scan.text;
                if (json_skip_value(&scan) != 0) {
                    forge_util_set_error(error, error_size,
                              "recipe feature '%s' has a malformed "
                              "dependency", def->name);
                    return -1;
                }
                dep_length = (size_t)(scan.text - dep_start);
                if (dep_length >= sizeof(dep_span)) {
                    forge_util_set_error(error, error_size,
                              "recipe feature '%s' has an oversized "
                              "dependency", def->name);
                    return -1;
                }
                memcpy(dep_span, dep_start, dep_length);
                dep_span[dep_length] = '\0';
                if (parse_feature_dep(dep_span, def, error, error_size) != 0) {
                    return -1;
                }
                json_skip_space(&scan);
                if (*scan.text == ',') {
                    ++scan.text;
                    continue;
                }
                if (*scan.text == ']') {
                    break;
                }
                forge_util_set_error(error, error_size,
                          "recipe feature '%s' has a malformed dependency "
                          "list", def->name);
                return -1;
            }
            break;
        }
    }
    ++defs->count;
    return 0;
}

/*
 * Reads the recipe's feature definitions ("features" array plus the
 * "default-features" names) into `defs`, zeroed first. Absent sections
 * mean "no features". Malformed shapes, bad names, over-cap counts, bad
 * versions, duplicate features/sections, or defaults naming undefined
 * features fail loudly. Returns 0 on success.
 */
int forge_registry_parse_features(const char *body, ForgeFeatureDefs *defs,
                                  char *error, size_t error_size)
{
    ForgeJsonCursor cursor = { body, NULL };
    char defaults[FORGE_DEP_FEATURES_MAX][FORGE_PATH_MAX];
    size_t default_count = 0U;
    int found_features = 0;
    int found_defaults = 0;
    int first = 1;
    size_t index;
    size_t slot;

    if (defs == NULL) {
        forge_util_set_error(error, error_size, "feature definitions output is required");
        return -1;
    }
    memset(defs, 0, sizeof(*defs));
    json_skip_space(&cursor);
    if (*cursor.text != '{') {
        forge_util_set_error(error, error_size, "registry response is not valid JSON");
        return -1;
    }
    ++cursor.text;
    for (;;) {
        char key[128];

        json_skip_space(&cursor);
        if (*cursor.text == '}') {
            break;
        }
        if (!first) {
            if (*cursor.text != ',') {
                forge_util_set_error(error, error_size, "registry response is not valid JSON");
                return -1;
            }
            ++cursor.text;
            json_skip_space(&cursor);
        }
        first = 0;
        if (json_read_string(&cursor, key, sizeof(key)) != 0) {
            forge_util_set_error(error, error_size, "registry response is not valid JSON");
            return -1;
        }
        json_skip_space(&cursor);
        if (*cursor.text != ':') {
            forge_util_set_error(error, error_size, "registry response is not valid JSON");
            return -1;
        }
        ++cursor.text;
        json_skip_space(&cursor);
        if (strcmp(key, "features") == 0) {
            const char *span_start;
            char span[8192];
            size_t span_length;

            if (found_features) {
                forge_util_set_error(error, error_size, "registry response repeats features");
                return -1;
            }
            found_features = 1;
            if (*cursor.text != '[') {
                forge_util_set_error(error, error_size, "registry features are not an array");
                return -1;
            }
            ++cursor.text;
            for (;;) {
                json_skip_space(&cursor);
                if (*cursor.text == ']') {
                    ++cursor.text;
                    break;
                }
                if (*cursor.text != '{') {
                    forge_util_set_error(error, error_size, "registry feature is malformed");
                    return -1;
                }
                span_start = cursor.text;
                if (json_skip_value(&cursor) != 0) {
                    forge_util_set_error(error, error_size, "registry feature is malformed");
                    return -1;
                }
                span_length = (size_t)(cursor.text - span_start);
                if (span_length >= sizeof(span)) {
                    forge_util_set_error(error, error_size, "registry feature is oversized");
                    return -1;
                }
                memcpy(span, span_start, span_length);
                span[span_length] = '\0';
                if (parse_feature(span, defs, error, error_size) != 0) {
                    return -1;
                }
                json_skip_space(&cursor);
                if (*cursor.text == ',') {
                    ++cursor.text;
                    continue;
                }
                if (*cursor.text == ']') {
                    /* Consume the closer like the empty-array exit above:
                     * the outer object walk resumes after the array. */
                    ++cursor.text;
                    break;
                }
                forge_util_set_error(error, error_size, "registry feature list is malformed");
                return -1;
            }
        } else if (strcmp(key, "default-features") == 0) {
            if (found_defaults) {
                forge_util_set_error(error, error_size, "registry response repeats default-features");
                return -1;
            }
            found_defaults = 1;
            if (json_array_strings(body, "default-features", defaults,
                                   FORGE_DEP_FEATURES_MAX,
                                   &default_count) != 0 ||
                json_skip_value(&cursor) != 0) {
                forge_util_set_error(error, error_size, "registry default-features are not a string array");
                return -1;
            }
        } else {
            if (json_skip_value(&cursor) != 0) {
                forge_util_set_error(error, error_size, "registry response is not valid JSON");
                return -1;
            }
        }
    }
    for (index = 0U; index < default_count; ++index) {
        if (!forge_feature_name_is_valid(defaults[index])) {
            forge_util_set_error(error, error_size,
                      "registry default feature '%s' is not a usable name",
                      defaults[index]);
            return -1;
        }
        for (slot = 0U; slot < defs->count; ++slot) {
            if (strcmp(defs->items[slot].name, defaults[index]) == 0) {
                break;
            }
        }
        if (slot == defs->count) {
            forge_util_set_error(error, error_size,
                      "registry default feature '%s' is not defined",
                      defaults[index]);
            return -1;
        }
        {
            size_t name_length = strlen(defaults[index]);

            memcpy(defs->defaults[defs->default_count], defaults[index],
                   name_length + 1U);
        }
        ++defs->default_count;
    }
    return 0;
}

int forge_features_join(size_t count,
                        const char names[][FORGE_FEATURE_NAME_MAX + 1U],
                        char *out, size_t out_size)
{
    char ordered[FORGE_DEP_FEATURES_MAX + FORGE_RECIPE_FEATURES_MAX][FORGE_FEATURE_NAME_MAX + 1U];
    size_t kept = 0U;
    size_t used = 0U;
    size_t slot;
    size_t index;

    if (out == NULL || out_size == 0U) {
        return -1;
    }
    out[0] = '\0';
    if (count > FORGE_DEP_FEATURES_MAX + FORGE_RECIPE_FEATURES_MAX) {
        return -1;
    }
    for (slot = 0U; slot < count; ++slot) {
        /* Overlong names never reach fixed buffers silently. */
        if (strlen(names[slot]) > FORGE_FEATURE_NAME_MAX) {
            return -1;
        }
        memcpy(ordered[slot], names[slot], strlen(names[slot]) + 1U);
    }
    /* Insertion sort plus dedupe into canonical order (memmove: the slots
     * overlap by construction, which snprintf forbids). */
    for (slot = 1U; slot < count; ++slot) {
        char held[FORGE_FEATURE_NAME_MAX + 1U];
        size_t held_at = slot;

        memcpy(held, ordered[slot], sizeof(held));
        while (held_at > 0U && strcmp(ordered[held_at - 1U], held) > 0) {
            memmove(ordered[held_at], ordered[held_at - 1U],
                    sizeof(ordered[held_at]));
            --held_at;
        }
        memcpy(ordered[held_at], held, sizeof(held));
    }
    for (slot = 0U; slot < count; ++slot) {
        if (kept != 0U &&
            strcmp(ordered[slot], ordered[kept - 1U]) == 0) {
            continue;
        }
        if (slot != kept) {
            memmove(ordered[kept], ordered[slot], sizeof(ordered[kept]));
        }
        ++kept;
    }
    for (index = 0U; index < kept; ++index) {
        size_t length = strlen(ordered[index]);

        if (used + length + (index != 0U ? 1U : 0U) + 1U > out_size) {
            return -1;
        }
        if (index != 0U) {
            out[used++] = ',';
        }
        memcpy(out + used, ordered[index], length);
        used += length;
        out[used] = '\0';
    }
    return 0;
}

int forge_features_effective(const ForgeFeatureDefs *defs,
                             const char declared[][FORGE_FEATURE_NAME_MAX + 1U],
                             size_t ndeclared, int use_defaults,
                             const char *dep_name,
                             char *out, size_t out_size,
                             char *error, size_t error_size)
{
    char merged[FORGE_DEP_FEATURES_MAX + FORGE_RECIPE_FEATURES_MAX][FORGE_FEATURE_NAME_MAX + 1U];
    size_t nmerged = 0U;
    size_t index;
    size_t slot;

    if (defs == NULL || out == NULL) {
        forge_util_set_error(error, error_size, "feature definitions and output are required");
        return -1;
    }
    for (index = 0U; index < ndeclared; ++index) {
        for (slot = 0U; slot < defs->count; ++slot) {
            if (strcmp(defs->items[slot].name, declared[index]) == 0) {
                break;
            }
        }
        if (slot == defs->count) {
            if (defs->count == 0U) {
                forge_util_set_error(error, error_size,
                          "dependency '%s': unknown feature '%s' (the recipe "
                          "defines no features)",
                          dep_name != NULL ? dep_name : "?",
                          declared[index]);
            } else {
                char available[512];
                size_t used = 0U;
                size_t pick;

                available[0] = '\0';
                for (pick = 0U; pick < defs->count; ++pick) {
                    size_t length = strlen(defs->items[pick].name);

                    if (used + length + 2U >= sizeof(available)) {
                        break;
                    }
                    if (used != 0U) {
                        available[used++] = ',';
                        available[used++] = ' ';
                    }
                    memcpy(available + used, defs->items[pick].name, length);
                    used += length;
                    available[used] = '\0';
                }
                forge_util_set_error(error, error_size,
                          "dependency '%s': unknown feature '%s' "
                          "(available: %s)",
                          dep_name != NULL ? dep_name : "?",
                          declared[index], available);
            }
            return -1;
        }
        (void)snprintf(merged[nmerged], sizeof(merged[nmerged]), "%s",
                       declared[index]);
        ++nmerged;
    }
    if (use_defaults) {
        for (slot = 0U; slot < defs->default_count; ++slot) {
            (void)snprintf(merged[nmerged], sizeof(merged[nmerged]), "%s",
                           defs->defaults[slot]);
            ++nmerged;
        }
    }
    if (forge_features_join(nmerged, merged, out, out_size) != 0) {
        forge_util_set_error(error, error_size,
                  "dependency '%s': feature set is too large",
                  dep_name != NULL ? dep_name : "?");
        return -1;
    }
    return 0;
}

/* ------------------------------------------------------------------ */
/* Array selection (legacy helper retained for static registry indexes) */
/* ------------------------------------------------------------------ */

/*
 * Scans the array at top-level `array_key` for the first object whose
 * `match_key` equals `match_value`, then reads `want_a`/`want_b` from it.
 * Returns 1 on match, 0 when the array or a match is absent, -1 on
 * malformed input. Object spans are capped (loud error past the cap).
 */
static int json_array_select(const char *json, const char *array_key,
                             const char *match_key, const char *match_value,
                             const char *want_a, char *out_a, size_t a_size,
                             const char *want_b, char *out_b, size_t b_size)
{
    ForgeJsonCursor cursor = { json, NULL };
    int first = 1;
    int found_array = 0;

    json_skip_space(&cursor);
    if (*cursor.text != '{') {
        return -1;
    }
    ++cursor.text;
    /* Locate the array value without consuming the rest of the object. */
    for (;;) {
        char key[128];

        json_skip_space(&cursor);
        if (*cursor.text == '}') {
            return 0;
        }
        if (!first) {
            if (*cursor.text != ',') {
                return -1;
            }
            ++cursor.text;
            json_skip_space(&cursor);
        }
        first = 0;
        if (json_read_string(&cursor, key, sizeof(key)) != 0) {
            return -1;
        }
        json_skip_space(&cursor);
        if (*cursor.text != ':') {
            return -1;
        }
        ++cursor.text;
        json_skip_space(&cursor);
        if (strcmp(key, array_key) == 0) {
            found_array = 1;
            break;
        }
        if (json_skip_value(&cursor) != 0) {
            return -1;
        }
    }
    if (!found_array || *cursor.text != '[') {
        return 0;
    }
    ++cursor.text;
    for (;;) {
        const char *span_start;
        char span[8192];
        size_t span_length;
        ForgeJsonCursor object;
        char candidate[FORGE_PATH_MAX];
        int is_null = 0;
        int matched;

        json_skip_space(&cursor);
        if (*cursor.text == ']') {
            return 0;
        }
        if (*cursor.text != '{') {
            return -1;
        }
        span_start = cursor.text;
        if (json_skip_value(&cursor) != 0) {
            return -1;
        }
        span_length = (size_t)(cursor.text - span_start);
        if (span_length >= sizeof(span)) {
            return -1;
        }
        memcpy(span, span_start, span_length);
        span[span_length] = '\0';
        object.text = span;
        object.error = NULL;
        matched = json_object_string(&object, match_key, candidate,
                                     sizeof(candidate), &is_null);
        if (matched < 0) {
            return -1;
        }
        if (matched == 0 || is_null ||
            strcmp(candidate, match_value) != 0) {
            json_skip_space(&cursor);
            if (*cursor.text == ',') {
                ++cursor.text;
            }
            continue;
        }
        object.text = span;
        object.error = NULL;
        matched = json_object_string(&object, want_a, out_a, a_size,
                                     &is_null);
        if (matched <= 0 || is_null) {
            return -1;
        }
        object.text = span;
        object.error = NULL;
        matched = json_object_string(&object, want_b, out_b, b_size,
                                     &is_null);
        if (matched <= 0 || is_null) {
            return -1;
        }
        return 1;
    }
}

/* ------------------------------------------------------------------ */
/* Static file registries (mirrors, air-gap, tests)                    */
/*                                                                     */
/* A file:// base points at the SITE root, exactly like an https://    */
/* base: every path joins the same way ("/packages/..." + base). The   */
/* only difference is the absence of query strings, so version         */
/* selection and triplet matching happen client-side over the same     */
/* JSON shapes the HTTP API serves, with the same strictness.          */
/* ------------------------------------------------------------------ */

static int query_file_registry(const char *base, const char *package,
                               const char *version,
                               ForgeRegistryPin *pin, ForgeFeatureDefs *defs,
                               char *error, size_t error_size)
{
    /* base is policy-checked already; strip the scheme, reject hosts. */
    char root[FORGE_PATH_MAX];
    char path[FORGE_PATH_MAX];
    char body[65536];
    char latest[FORGE_MANIFEST_VALUE_MAX];
    char index[FORGE_PATH_MAX];
    const char *dir = base + 7U;
    const char *wanted = version;
    char display_version[FORGE_MANIFEST_VALUE_MAX];
    int found;

    if (dir[0] != '/') {
        forge_util_set_error(error, error_size,
                  "file:// registries must point at a local directory "
                  "(file:///path); host shares are not supported");
        return -1;
    }
    if (snprintf(root, sizeof(root), "%s", dir) < 0 ||
        strlen(dir) >= sizeof(root)) {
        forge_util_set_error(error, error_size, "registry path is too long");
        return -1;
    }
    if (root[0] == '/' && isalpha((unsigned char)root[1]) && root[2] == ':') {
        memmove(root, root + 1U, strlen(root));
    }
    if (wanted == NULL || wanted[0] == '\0') {
        /* Newest release comes from the index. */
        if (snprintf(path, sizeof(path), "%s/packages/sunn.registry.json",
                     root) < 0 ||
            strlen(root) + 29U >= sizeof(path)) {
            forge_util_set_error(error, error_size, "registry path is too long");
            return -1;
        }
        if (read_response_file(path, body, sizeof(body), error,
                               error_size) != 0) {
            return -1;
        }
        if (body[0] != '{') {
            forge_util_set_error(error, error_size,
                      "registry index '%s' is not valid JSON", path);
            return -1;
        }
        found = json_array_select(body, "packages", "name", package,
                                  "latest", latest, sizeof(latest),
                                  "index", index, sizeof(index));
        if (found < 0) {
            forge_util_set_error(error, error_size,
                      "registry index '%s' is not valid JSON", path);
            return -1;
        }
        if (found == 0) {
            forge_util_set_error(error, error_size,
                      "registry has no package '%s'", package);
            return -1;
        }
        /* The index field is site-absolute ("/packages/..."); it must
         * stay inside the registry — refuse escapes loudly. */
        if (index[0] != '/' || strstr(index, "..") != NULL ||
            snprintf(path, sizeof(path), "%s%s", root, index) < 0 ||
            strlen(root) + strlen(index) >= sizeof(path)) {
            forge_util_set_error(error, error_size,
                      "registry index '%s' has no usable entry for '%s'",
                      path, package);
            return -1;
        }
        (void)snprintf(display_version, sizeof(display_version), "%s", latest);
    } else {
        (void)snprintf(display_version, sizeof(display_version), "%s", wanted);
        if (snprintf(path, sizeof(path), "%s/packages/%s/%s.json", root,
                     package, wanted) < 0 ||
            strlen(root) + strlen(package) + strlen(wanted) + 18U >=
                sizeof(path)) {
            forge_util_set_error(error, error_size, "registry path is too long");
            return -1;
        }
    }
    {
        FILE *probe = fopen(path, "rb");

        if (probe == NULL) {
            forge_util_set_error(error, error_size,
                      "registry has no package '%s' version '%s'", package,
                      display_version);
            return -1;
        }
        (void)fclose(probe);
    }
    if (read_response_file(path, body, sizeof(body), error, error_size) != 0) {
        return -1;
    }
    if (body[0] != '{') {
        forge_util_set_error(error, error_size,
                  "registry file '%s' is not valid JSON", path);
        return -1;
    }
    {
        if (parse_recipe(body, base, pin, error, error_size) != 0) return -1;
        if (forge_registry_parse_features(body, defs, error, error_size) != 0) {
            forge_util_prepend_error(error, error_size,
                                     "registry recipe for '%s': ", package);
            return -1;
        }
    }
    return 0;
}

/* ------------------------------------------------------------------ */
/* Resolve endpoint                                                    */
/* ------------------------------------------------------------------ */

static int read_response_file(const char *path, char *body, size_t body_size,
                              char *error, size_t error_size)
{
    FILE *file;
    size_t total = 0U;
    size_t read_bytes;

    file = fopen(path, "rb");
    if (file == NULL) {
        forge_util_set_error(error, error_size,
                  "cannot read registry response '%s'", path);
        return -1;
    }
    do {
        read_bytes = fread(body + total, 1U, body_size - 1U - total, file);
        total += read_bytes;
    } while (read_bytes > 0U && total < body_size - 1U);
    if (ferror(file)) {
        forge_util_set_error(error, error_size,
                  "cannot read registry response '%s'", path);
        (void)fclose(file);
        return -1;
    }
    (void)fclose(file);
    if (total >= body_size - 1U) {
        forge_util_set_error(error, error_size,
                  "registry response '%s' is too large", path);
        return -1;
    }
    body[total] = '\0';
    return 0;
}

/* ------------------------------------------------------------------ */
/* Registry baseline (minimum floor for floating entries)              */
/*                                                                     */
/* baseline.json at the registry root pins the minimum (version,       */
/* revision) per package, vcpkg-baseline style. Exact manifest pins    */
/* bypass it; minimums and bare entries resolve no lower. A registry   */
/* without the file (or without an entry) simply has no floor.         */
/* ------------------------------------------------------------------ */

/*
 * Looks up `package` in a baseline.json body. Returns 1 with the floor in
 * `version_out`/`revision_out`, 0 when the package has no entry, -1 on
 * malformed input. A missing revision means 0.
 */
static int baseline_lookup(const char *body, const char *package,
                           char *version_out, size_t version_size,
                           unsigned *revision_out)
{
    ForgeJsonCursor cursor = { body, NULL };
    int first = 1;

    json_skip_space(&cursor);
    if (*cursor.text != '{') {
        return -1;
    }
    ++cursor.text;
    /* Locate the "baseline" array. */
    for (;;) {
        char key[128];

        json_skip_space(&cursor);
        if (*cursor.text == '}') {
            return 0;
        }
        if (!first) {
            if (*cursor.text != ',') {
                return -1;
            }
            ++cursor.text;
            json_skip_space(&cursor);
        }
        first = 0;
        if (json_read_string(&cursor, key, sizeof(key)) != 0) {
            return -1;
        }
        json_skip_space(&cursor);
        if (*cursor.text != ':') {
            return -1;
        }
        ++cursor.text;
        json_skip_space(&cursor);
        if (strcmp(key, "baseline") == 0) {
            break;
        }
        if (json_skip_value(&cursor) != 0) {
            return -1;
        }
    }
    if (*cursor.text != '[') {
        return -1;
    }
    ++cursor.text;
    for (;;) {
        const char *span_start;
        char span[2048];
        size_t span_length;
        ForgeJsonCursor object;
        char candidate[FORGE_PATH_MAX];
        char entry_version[FORGE_MANIFEST_VALUE_MAX];
        unsigned entry_revision = 0U;
        int is_null = 0;
        int matched;

        json_skip_space(&cursor);
        if (*cursor.text == ']') {
            return 0;
        }
        if (*cursor.text != '{') {
            return -1;
        }
        span_start = cursor.text;
        if (json_skip_value(&cursor) != 0) {
            return -1;
        }
        span_length = (size_t)(cursor.text - span_start);
        if (span_length >= sizeof(span)) {
            return -1;
        }
        memcpy(span, span_start, span_length);
        span[span_length] = '\0';
        object.text = span;
        object.error = NULL;
        matched = json_object_string(&object, "name", candidate,
                                     sizeof(candidate), &is_null);
        if (matched < 0) {
            return -1;
        }
        if (matched == 0 || is_null || strcmp(candidate, package) != 0) {
            json_skip_space(&cursor);
            if (*cursor.text == ',') {
                ++cursor.text;
            }
            continue;
        }
        object.text = span;
        object.error = NULL;
        matched = json_object_string(&object, "version", entry_version,
                                     sizeof(entry_version), &is_null);
        if (matched <= 0 || is_null || !version_text_is_valid(entry_version)) {
            return -1;
        }
        object.text = span;
        object.error = NULL;
        matched = json_object_uint(&object, "revision", &entry_revision,
                                   FORGE_REGISTRY_MAX_REVISION, &is_null);
        if (matched < 0) {
            return -1;
        }
        if ((size_t)snprintf(version_out, version_size, "%s",
                             entry_version) >= version_size) {
            return -1;
        }
        *revision_out = (matched > 0 && !is_null) ? entry_revision : 0U;
        return 1;
    }
}

/*
 * Reads the baseline floor for `package`: an empty `version_out` (with
 * revision 0) means the registry states no floor. A missing baseline.json
 * on a file:// registry predates baselines and is not an error; over HTTP
 * every failure is loud so a misconfigured registry cannot silently float
 * pins to newest. --offline always fails: a floor the lockfile cannot
 * vouch for must never resolve.
 */
static int fetch_baseline(ForgeLogger *logger, const char *base,
                          const char *package, const char *tmp_path,
                          int offline, const char *dep_name,
                          char *version_out, size_t version_size,
                          unsigned *revision_out,
                          char *error, size_t error_size)
{
    char path[FORGE_PATH_MAX * 2U];
    char body[65536];
    int looked_up;

    version_out[0] = '\0';
    *revision_out = 0U;
    if (offline) {
        forge_util_set_error(error, error_size,
                  "dependency '%s': registry baseline is not cached and "
                  "--offline forbids fetching it",
                  dep_name != NULL ? dep_name : package);
        return -1;
    }
    if (strncmp(base, "file://", 7U) == 0) {
        char root[FORGE_PATH_MAX];
        const char *dir = base + 7U;
        FILE *probe;

        if (dir[0] != '/') {
            forge_util_set_error(error, error_size,
                      "file:// registries must point at a local directory "
                      "(file:///path); host shares are not supported");
            return -1;
        }
        if (snprintf(root, sizeof(root), "%s", dir) < 0 ||
            strlen(dir) >= sizeof(root)) {
            forge_util_set_error(error, error_size, "registry path is too long");
            return -1;
        }
        if (root[0] == '/' && isalpha((unsigned char)root[1]) && root[2] == ':') {
            memmove(root, root + 1U, strlen(root));
        }
        if (snprintf(path, sizeof(path), "%s/baseline.json", root) < 0 ||
            strlen(root) + 14U >= sizeof(path)) {
            forge_util_set_error(error, error_size, "registry path is too long");
            return -1;
        }
        probe = fopen(path, "rb");
        if (probe == NULL) {
            return 0;
        }
        (void)fclose(probe);
        if (read_response_file(path, body, sizeof(body), error,
                               error_size) != 0) {
            return -1;
        }
    } else {
        if (snprintf(path, sizeof(path), "%s/baseline.json", base) < 0 ||
            strlen(base) + 14U >= sizeof(path)) {
            forge_util_set_error(error, error_size, "registry query URL is too long");
            return -1;
        }
        if (forge_fetch_url_is_supported(path, error, error_size) != 0 ||
            forge_fetch_to_file(logger, path, tmp_path, error, error_size) != 0 ||
            read_response_file(tmp_path, body, sizeof(body), error,
                               error_size) != 0) {
            return -1;
        }
    }
    if (body[0] != '{') {
        forge_util_set_error(error, error_size,
                  "registry baseline '%s' is not valid JSON", path);
        return -1;
    }
    looked_up = baseline_lookup(body, package, version_out, version_size,
                                revision_out);
    if (looked_up < 0) {
        forge_util_set_error(error, error_size,
                  "registry baseline '%s' is not valid JSON", path);
        return -1;
    }
    return 0;
}

/*
 * Computes the (version, revision) floor for a floating entry: the manifest
 * minimum raised to the registry baseline. An empty floor version means
 * "no floor", and bare entries then track newest. Fails closed.
 */
static int resolve_floor(ForgeLogger *logger, const char *dep_name,
                         const char *base, const char *package,
                         const char *min_version, const char *tmp_path,
                         int offline, char *floor_version, size_t floor_size,
                         unsigned *floor_revision,
                         char *error, size_t error_size)
{
    char baseline_version[FORGE_MANIFEST_VALUE_MAX] = { 0 };
    unsigned baseline_revision = 0U;
    int order;

    floor_version[0] = '\0';
    *floor_revision = 0U;
    if (min_version != NULL && min_version[0] != '\0') {
        (void)snprintf(floor_version, floor_size, "%s", min_version);
    }
    if (fetch_baseline(logger, base, package, tmp_path, offline, dep_name,
                       baseline_version, sizeof(baseline_version),
                       &baseline_revision, error, error_size) != 0) {
        return -1;
    }
    if (baseline_version[0] == '\0') {
        return 0;
    }
    if (floor_version[0] == '\0') {
        (void)snprintf(floor_version, floor_size, "%s", baseline_version);
        *floor_revision = baseline_revision;
        return 0;
    }
    order = forge_version_compare(baseline_version, floor_version);
    if (order > 0 ||
        (order == 0 && baseline_revision > *floor_revision)) {
        (void)snprintf(floor_version, floor_size, "%s", baseline_version);
        *floor_revision = baseline_revision;
    }
    return 0;
}

int forge_registry_query(ForgeLogger *logger, const char *package,
                         const char *version, const char *tmp_path,
                         ForgeRegistryPin *pin, ForgeFeatureDefs *defs,
                         char *error, size_t error_size)
{
    char base[FORGE_PATH_MAX];
    char url[FORGE_PATH_MAX * 2U];
    char body[65536];
    char value[FORGE_PATH_MAX];
    int is_null = 0;
    int found;

    if (pin == NULL || defs == NULL || tmp_path == NULL) {
        forge_util_set_error(error, error_size, "registry query needs a pin, feature definitions, and scratch space");
        return -1;
    }
    memset(pin, 0, sizeof(*pin));
    memset(defs, 0, sizeof(*defs));
    if (!package_name_is_portable(package)) {
        forge_util_set_error(error, error_size,
                  "'%s' is not a valid registry package name; use letters, "
                  "digits, '-', '_', '.'",
                  package != NULL ? package : "<null>");
        return -1;
    }
    if (version != NULL && version[0] != '\0' &&
        !version_text_is_valid(version)) {
        forge_util_set_error(error, error_size,
                  "'%s' is not a valid registry version", version);
        return -1;
    }
    if (forge_registry_base_url(base, sizeof(base), error, error_size) != 0) {
        return -1;
    }
    /* Overlays shadow every transport: first overlay naming the recipe
     * wins, so local patches and proprietary ports resolve without a
     * registry round-trip. */
    {
        char roots[FORGE_REGISTRY_MAX_OVERLAYS][FORGE_PATH_MAX];
        size_t nroots = 0U;
        size_t i;

        if (overlay_roots(roots, FORGE_REGISTRY_MAX_OVERLAYS, &nroots) != 0) {
            forge_util_set_error(error, error_size,
                      "FORGE_OVERLAYS names a path that is too long or "
                      "escapes with '..'");
            return -1;
        }
        for (i = 0U; i < nroots; ++i) {
            if (version != NULL && version[0] != '\0') {
                ForgeRegistryPin over = {0};
                ForgeFeatureDefs odefs = {0};

                if (overlay_try_recipe(roots[i], package, version, &over,
                                       &odefs, error, error_size)) {
                    *pin = over;
                    *defs = odefs;
                    return 0;
                }
            } else {
                char path[FORGE_PATH_MAX];
                char index_body[65536];
                char latest[FORGE_MANIFEST_VALUE_MAX];
                char index_ptr[FORGE_PATH_MAX];
                FILE *probe;

                if ((size_t)snprintf(path, sizeof(path),
                                     "%s/packages/sunn.registry.json",
                                     roots[i]) >= sizeof(path)) {
                    continue;
                }
                probe = fopen(path, "rb");
                if (probe == NULL) {
                    continue;
                }
                (void)fclose(probe);
                if (read_response_file(path, index_body, sizeof(index_body),
                                       error, error_size) != 0) {
                    continue;
                }
                if (index_body[0] != '{') {
                    continue;
                }
                if (json_array_select(index_body, "packages", "name",
                                      package, "latest", latest,
                                      sizeof(latest), "index", index_ptr,
                                      sizeof(index_ptr)) > 0) {
                    ForgeRegistryPin over = {0};
                    ForgeFeatureDefs odefs = {0};

                    if (overlay_try_recipe(roots[i], package, latest,
                                           &over, &odefs, error,
                                           error_size)) {
                        *pin = over;
                        *defs = odefs;
                        return 0;
                    }
                }
            }
        }
    }
    if (strncmp(base, "file://", 7U) == 0) {
        /* Static layout: no query strings exist for files. */
        (void)logger;
        (void)tmp_path;
        return query_file_registry(base, package, version, pin, defs,
                                   error, error_size);
    }
    /* snprintf truncations are detected via the would-be length: a silently
     * shortened query URL would resolve the wrong package. */
    {
        int written;

        if (version != NULL && version[0] != '\0') {
            written = snprintf(url, sizeof(url),
                               "%s/api/forge/v1/resolve?name=%s&version=%s",
                               base, package, version);
        } else {
            written = snprintf(url, sizeof(url),
                               "%s/api/forge/v1/resolve?name=%s",
                               base, package);
        }
        if (written < 0 || (size_t)written >= sizeof(url)) {
            forge_util_set_error(error, error_size, "registry query URL is too long");
            return -1;
        }
    }
    /* The endpoint URL is built from validated pieces, but run it through
     * the same transport policy as artifact downloads anyway. */
    if (forge_fetch_url_is_supported(url, error, error_size) != 0) {
        return -1;
    }
    if (forge_fetch_to_file(logger, url, tmp_path, error, error_size) != 0) {
        return -1;
    }
    if (read_response_file(tmp_path, body, sizeof(body), error,
                           error_size) != 0) {
        return -1;
    }
    /* A server-side refusal arrives as {"error": "..."}; surface it. */
    found = json_object_string(&(ForgeJsonCursor){ body, NULL }, "error",
                               value, sizeof(value), &is_null);
    if (found < 0) {
        forge_util_set_error(error, error_size,
                  "registry response for '%s' is not valid JSON", package);
        return -1;
    }
    if (found > 0 && !is_null) {
        forge_util_set_error(error, error_size, "registry refused '%s': %s",
                  package, value);
        return -1;
    }
    found = json_object_string(&(ForgeJsonCursor){ body, NULL }, "version",
                               pin->version, sizeof(pin->version), &is_null);
    if (found <= 0 || is_null || !version_text_is_valid(pin->version)) {
        forge_util_set_error(error, error_size,
                  "registry response for '%s' has no usable version", package);
        return -1;
    }
    if (parse_recipe(body, base, pin, error, error_size) != 0) {
        forge_util_prepend_error(error, error_size,
                                  "registry response for '%s': ", package);
        return -1;
    }
    if (forge_registry_parse_features(body, defs, error, error_size) != 0) {
        forge_util_prepend_error(error, error_size,
                                  "registry response for '%s': ", package);
        return -1;
    }
    return 0;
}

