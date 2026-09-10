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

void forge_registry_host_triplet(char *triplet_out, size_t triplet_size)
{
#if defined(_M_ARM64) || defined(__aarch64__)
    const char *arch = "aarch64";
    const char *label = "arm64";
#elif defined(_M_X64) || defined(__x86_64__) || defined(_M_AMD64)
    const char *arch = "x86_64";
    const char *label = "x64";
#else
    const char *arch = "unknown";
    const char *label = "unknown";
#endif
#if FORGE_PLATFORM_WINDOWS
    const char *os = "windows";
#elif defined(__APPLE__)
    const char *os = "macos";
#elif defined(__linux__)
    const char *os = "linux";
#else
    const char *os = "unknown";
#endif

    (void)arch;
    (void)snprintf(triplet_out, triplet_size, "%s-%s", label, os);
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

typedef struct ForgeJsonCursor {
    const char *text;
    const char *error;
} ForgeJsonCursor;

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

/* ------------------------------------------------------------------ */
/* Array selection (file-layout registries list per-triplet artifacts) */
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
                               const char *version, const char *triplet,
                               ForgeRegistryPin *pin,
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
        char file_version[FORGE_MANIFEST_VALUE_MAX];
        char url[FORGE_PATH_MAX];
        char absolute[FORGE_PATH_MAX];
        int is_null = 0;

        found = json_object_string(&(ForgeJsonCursor){ body, NULL }, "version",
                                   file_version, sizeof(file_version),
                                   &is_null);
        if (found <= 0 || is_null || !version_text_is_valid(file_version)) {
            forge_util_set_error(error, error_size,
                      "registry file '%s' has no usable version", path);
            return -1;
        }
        found = json_array_select(body, "artifacts", "triplet", triplet,
                                  "url", url, sizeof(url),
                                  "sha256", pin->sha256,
                                  sizeof(pin->sha256));
        if (found < 0) {
            forge_util_set_error(error, error_size,
                      "registry file '%s' is not valid JSON", path);
            return -1;
        }
        if (found == 0) {
            forge_util_set_error(error, error_size,
                      "registry has no '%s' build for triplet '%s'", package,
                      triplet);
            return -1;
        }
        if (!sha256_text_is_valid(pin->sha256)) {
            forge_util_set_error(error, error_size,
                      "registry file '%s' has no usable sha256", path);
            return -1;
        }
        if (url[0] == '/' && url[1] != '/') {
            if (snprintf(absolute, sizeof(absolute), "%s%s", base, url) < 0 ||
                strlen(base) + strlen(url) >= sizeof(absolute)) {
                forge_util_set_error(error, error_size, "registry artifact URL is too long");
                return -1;
            }
        } else {
            if (snprintf(absolute, sizeof(absolute), "%s", url) < 0 ||
                strlen(url) >= sizeof(absolute)) {
                forge_util_set_error(error, error_size, "registry artifact URL is too long");
                return -1;
            }
        }
        if (forge_fetch_url_is_supported(absolute, error, error_size) != 0) {
            return -1;
        }
        (void)snprintf(pin->version, sizeof(pin->version), "%s", file_version);
        (void)snprintf(pin->url, sizeof(pin->url), "%s", absolute);
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

int forge_registry_query(ForgeLogger *logger, const char *package,
                         const char *version, const char *triplet,
                         const char *tmp_path, ForgeRegistryPin *pin,
                         char *error, size_t error_size)
{
    char base[FORGE_PATH_MAX];
    char url[FORGE_PATH_MAX * 2U];
    char body[65536];
    char value[FORGE_PATH_MAX];
    char absolute[FORGE_PATH_MAX];
    int is_null = 0;
    int found;

    if (pin == NULL || tmp_path == NULL) {
        forge_util_set_error(error, error_size, "registry query needs a pin and scratch space");
        return -1;
    }
    memset(pin, 0, sizeof(*pin));
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
    if (triplet == NULL || triplet[0] == '\0') {
        forge_util_set_error(error, error_size, "registry query needs a triplet");
        return -1;
    }
    if (strncmp(base, "file://", 7U) == 0) {
        /* Static layout: no query strings exist for files. */
        (void)logger;
        (void)tmp_path;
        return query_file_registry(base, package, version, triplet, pin,
                                   error, error_size);
    }
    /* snprintf truncations are detected via the would-be length: a silently
     * shortened query URL would resolve the wrong package. */
    {
        int written;

        if (version != NULL && version[0] != '\0') {
            written = snprintf(url, sizeof(url),
                               "%s/api/forge/v1/resolve?name=%s&version=%s&triplet=%s",
                               base, package, version, triplet);
        } else {
            written = snprintf(url, sizeof(url),
                               "%s/api/forge/v1/resolve?name=%s&triplet=%s",
                               base, package, triplet);
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
    found = json_nested_string(body, "artifact", "url", value, sizeof(value),
                               &is_null);
    if (found < 0) {
        forge_util_set_error(error, error_size,
                  "registry response for '%s' is not valid JSON", package);
        return -1;
    }
    if (found == 0 || is_null || value[0] == '\0') {
        forge_util_set_error(error, error_size,
                  "registry has no '%s' build for triplet '%s'", package,
                  triplet);
        return -1;
    }
    /* Artifact URLs are either absolute https (CDN) or registry-relative
     * (our static hosting, "/packages/..."); join the latter onto base. */
    if (value[0] == '/' && value[1] != '/') {
        if (snprintf(absolute, sizeof(absolute), "%s%s", base, value) < 0 ||
            strlen(base) + strlen(value) >= sizeof(absolute)) {
            forge_util_set_error(error, error_size, "registry artifact URL is too long");
            return -1;
        }
    } else {
        if (snprintf(absolute, sizeof(absolute), "%s", value) < 0 ||
            strlen(value) >= sizeof(absolute)) {
            forge_util_set_error(error, error_size, "registry artifact URL is too long");
            return -1;
        }
    }
    if (forge_fetch_url_is_supported(absolute, error, error_size) != 0) {
        return -1;
    }
    (void)snprintf(pin->url, sizeof(pin->url), "%s", absolute);
    found = json_nested_string(body, "artifact", "sha256", pin->sha256,
                               sizeof(pin->sha256), &is_null);
    if (found < 0) {
        forge_util_set_error(error, error_size,
                  "registry response for '%s' is not valid JSON", package);
        return -1;
    }
    if (found == 0 || is_null || !sha256_text_is_valid(pin->sha256)) {
        forge_util_set_error(error, error_size,
                  "registry response for '%s' has no usable sha256", package);
        return -1;
    }
    return 0;
}

/* ------------------------------------------------------------------ */
/* Materialize: reuse-or-fetch into the shared cache                   */
/* ------------------------------------------------------------------ */

/* True when `directory` already holds an unpacked dependency source. */
static int directory_has_source(const char *directory)
{
    char probe[FORGE_PATH_MAX];
    static const char *const markers[] = {
        "Forge.toml", "CMakeLists.txt", "Makefile", "makefile", "GNUmakefile"
    };
    size_t index;
    FILE *file;

    for (index = 0U; index < sizeof(markers) / sizeof(markers[0]); ++index) {
        if (snprintf(probe, sizeof(probe), "%s/%s", directory,
                     markers[index]) < 0) {
            continue;
        }
        file = fopen(probe, "rb");
        if (file != NULL) {
            (void)fclose(file);
            return 1;
        }
    }
    return 0;
}

/* The sha recorded when this checkout was unpacked (empty when unknown). */
static void read_pin_marker(const char *version_dir, char *sha_out,
                            size_t sha_size)
{
    char path[FORGE_PATH_MAX];
    FILE *file;
    size_t length;

    sha_out[0] = '\0';
    if (snprintf(path, sizeof(path), "%s/.forge-pin-sha256", version_dir) < 0) {
        return;
    }
    file = fopen(path, "rb");
    if (file == NULL) {
        return;
    }
    length = fread(sha_out, 1U, sha_size - 1U, file);
    (void)fclose(file);
    sha_out[length] = '\0';
    sha_out[strcspn(sha_out, "\r\n")] = '\0';
}

static int write_pin_marker(const char *version_dir, const char *sha,
                            char *error, size_t error_size)
{
    char path[FORGE_PATH_MAX];
    FILE *file;

    if (snprintf(path, sizeof(path), "%s/.forge-pin-sha256", version_dir) < 0) {
        forge_util_set_error(error, error_size, "registry cache path is too long");
        return -1;
    }
    file = fopen(path, "wb");
    if (file == NULL) {
        forge_util_set_error(error, error_size,
                  "cannot record registry pin in '%s'", version_dir);
        return -1;
    }
    if (fputs(sha, file) < 0 || fputc('\n', file) == EOF ||
        fclose(file) != 0) {
        forge_util_set_error(error, error_size,
                  "cannot record registry pin in '%s'", version_dir);
        return -1;
    }
    return 0;
}

int forge_registry_materialize(ForgeLogger *logger, const char *dep_name,
                               const char *package,
                               const char *wanted_version,
                               const char *lock_version, const char *lock_sha,
                               const char *lock_url,
                               int force_update, int offline,
                               const char *package_dir,
                               char *root_out, size_t root_size,
                               ForgeRegistryPin *pin, int *reused,
                               char *error, size_t error_size)
{
    char version[FORGE_MANIFEST_VALUE_MAX];
    char safe_version[FORGE_PATH_MAX];
    char version_dir[FORGE_PATH_MAX];
    char tmp_path[FORGE_PATH_MAX];
    char triplet[64];
    char marker[FORGE_SHA256_HEX_LENGTH + 1U];
    const char *effective_lock_version;
    const char *effective_lock_sha;
    const char *effective_lock_url;
    int query_latest;

    if (pin == NULL || reused == NULL) {
        forge_util_set_error(error, error_size, "registry materialize needs a pin and reuse flag");
        return -1;
    }
    memset(pin, 0, sizeof(*pin));
    *reused = 0;
    if (!package_name_is_portable(package)) {
        forge_util_set_error(error, error_size,
                  "'%s' is not a valid registry package name",
                  package != NULL ? package : "<null>");
        return -1;
    }
    if (wanted_version != NULL && wanted_version[0] != '\0' &&
        !version_text_is_valid(wanted_version)) {
        forge_util_set_error(error, error_size,
                  "'%s' is not a valid registry version", wanted_version);
        return -1;
    }
    effective_lock_version = lock_version != NULL ? lock_version : "";
    effective_lock_sha = lock_sha != NULL ? lock_sha : "";
    effective_lock_url = lock_url != NULL ? lock_url : "";

    /*
     * Pin policy: an explicitly wanted version always wins. Otherwise the
     * lock pin stays (quiet, offline-friendly) unless something asks to
     * move past it (`forge update`, which sets force_update).
     */
    query_latest = 0;
    if (wanted_version != NULL && wanted_version[0] != '\0') {
        (void)snprintf(version, sizeof(version), "%s", wanted_version);
    } else if (effective_lock_version[0] != '\0' && !force_update) {
        (void)snprintf(version, sizeof(version), "%s", effective_lock_version);
    } else {
        version[0] = '\0';
        query_latest = 1;
    }
    forge_paths_safe_output_name(package, safe_version,
                                 sizeof(safe_version));
    if (snprintf(tmp_path, sizeof(tmp_path), "%s/.resolve.json.tmp",
                 package_dir) < 0 ||
        strlen(package_dir) + 19U >= sizeof(tmp_path)) {
        forge_util_set_error(error, error_size, "registry cache path is too long");
        return -1;
    }
    version_dir[0] = '\0';
    if (!query_latest) {
        forge_paths_safe_output_name(version, safe_version,
                                     sizeof(safe_version));
        if (snprintf(version_dir, sizeof(version_dir), "%s/%s", package_dir,
                     safe_version) < 0 ||
            strlen(package_dir) + 1U + strlen(safe_version) >= sizeof(version_dir)) {
            forge_util_set_error(error, error_size, "registry cache path is too long");
            return -1;
        }
    }
    forge_registry_host_triplet(triplet, sizeof(triplet));

    /* Fast path: same version pinned, checksum marker matches, sources
     * present, and nobody asked to move. No network touched. */
    if (version_dir[0] != '\0' && !force_update && effective_lock_sha[0] != '\0' &&
        strcmp(version, effective_lock_version) == 0) {
        read_pin_marker(version_dir, marker, sizeof(marker));
        if (strcmp(marker, effective_lock_sha) == 0 &&
            directory_has_source(version_dir)) {
            (void)snprintf(pin->version, sizeof(pin->version), "%s", version);
            (void)snprintf(pin->sha256, sizeof(pin->sha256), "%s",
                           effective_lock_sha);
            /* Reuse keeps the committed URL: the pin must round-trip
             * byte-identical or the next load fails its own gate. */
            (void)snprintf(pin->url, sizeof(pin->url), "%s",
                           effective_lock_url);
            if ((size_t)snprintf(root_out, root_size, "%s", version_dir) >= root_size) {
                forge_util_set_error(error, error_size, "registry cache path is too long");
                return -1;
            }
            *reused = 1;
            return 0;
        }
    }
    if (offline) {
        forge_util_set_error(error, error_size,
                  "dependency '%s': registry copy of '%s' is not cached and "
                  "--offline forbids fetching it",
                  dep_name != NULL ? dep_name : package, package);
        return -1;
    }
    if (forge_registry_query(logger, package, query_latest ? "" : version,
                             triplet, tmp_path, pin, error,
                             error_size) != 0) {
        return -1;
    }
    /* A lock pin that names a different checksum than the registry just
     * served means the bytes moved under a version: refuse loudly instead
     * of silently following. (A version change is decided by the caller
     * via wanted_version/force_update and the --locked gate.) */
    if (effective_lock_sha[0] != '\0' &&
        strcmp(pin->version, effective_lock_version) == 0 &&
        strcmp(pin->sha256, effective_lock_sha) != 0) {
        forge_util_set_error(error, error_size,
                  "dependency '%s': registry sha256 for '%s' version %s "
                  "changed under Forge.lock; delete the pin and run "
                  "'forge update' if the new bytes are trusted",
                  dep_name != NULL ? dep_name : package, package,
                  pin->version);
        return -1;
    }
    /* Recompute the version directory: "latest" may have resolved newer. */
    forge_paths_safe_output_name(pin->version, safe_version,
                                 sizeof(safe_version));
    if (snprintf(version_dir, sizeof(version_dir), "%s/%s", package_dir,
                 safe_version) < 0 ||
        strlen(package_dir) + 1U + strlen(safe_version) >= sizeof(version_dir)) {
        forge_util_set_error(error, error_size, "registry cache path is too long");
        return -1;
    }
    /* Same-version refresh that already matches needs no download. */
    if (!force_update) {
        read_pin_marker(version_dir, marker, sizeof(marker));
        if (strcmp(marker, pin->sha256) == 0 &&
            directory_has_source(version_dir)) {
            if ((size_t)snprintf(root_out, root_size, "%s", version_dir) >= root_size) {
                forge_util_set_error(error, error_size, "registry cache path is too long");
                return -1;
            }
            *reused = 1;
            return 0;
        }
    }
    {
        char archive[FORGE_PATH_MAX];
        char actual[FORGE_SHA256_HEX_LENGTH + 1U];

        if (snprintf(archive, sizeof(archive), "%s/.%s.tgz.tmp", package_dir,
                     safe_version) < 0 ||
            strlen(package_dir) + strlen(safe_version) + 11U >= sizeof(archive)) {
            forge_util_set_error(error, error_size, "registry cache path is too long");
            return -1;
        }
        forge_logger_detail(logger, "deps", "fetching %s %s", package,
                            pin->version);
        if (forge_fetch_to_file(logger, pin->url, archive, error,
                                error_size) != 0) {
            return -1;
        }
        if (forge_sha256_file(archive, actual, error, error_size) != 0) {
            (void)remove(archive);
            return -1;
        }
        if (strcmp(actual, pin->sha256) != 0) {
            forge_util_set_error(error, error_size,
                      "dependency '%s': downloaded sha256 %s does not match "
                      "the registry pin %s; deleted the download",
                      package, actual, pin->sha256);
            (void)remove(archive);
            return -1;
        }
        /* Verified bytes only from here on: replace any stale checkout. */
        forge_paths_remove_tree(version_dir, NULL, 0U);
        if (forge_fetch_unpack_tar_gz(logger, archive, version_dir, error,
                                      error_size) != 0) {
            (void)remove(archive);
            return -1;
        }
        (void)remove(archive);
        if (!directory_has_source(version_dir)) {
            forge_util_set_error(error, error_size,
                      "dependency '%s': package %s unpacked with no buildable "
                      "source (Forge.toml, CMakeLists.txt, or Makefile)",
                      package, pin->version);
            return -1;
        }
        if (write_pin_marker(version_dir, pin->sha256, error,
                             error_size) != 0) {
            return -1;
        }
    }
    forge_logger_detail(logger, "deps", "resolved %s %s", package,
                            pin->version);
    if ((size_t)snprintf(root_out, root_size, "%s", version_dir) >= root_size) {
        forge_util_set_error(error, error_size, "registry cache path is too long");
        return -1;
    }
    return 0;
}
