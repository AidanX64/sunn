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

int forge_registry_materialize(ForgeLogger *logger, const char *dep_name,
                               const char *package,
                               const char *wanted_version,
                               const char *min_version,
                               const char *lock_version, const char *lock_kind,
                               const char *lock_location, const char *lock_ref,
                               const char *lock_commit, const char *lock_sha256,
                               unsigned lock_revision,
                               int force_update, int offline,
                               const char *package_dir,
                               char *root_out, size_t root_size,
                               ForgeRegistryPin *pin, int *reused,
                               char *error, size_t error_size)
{
    char base[FORGE_PATH_MAX];
    char version[FORGE_MANIFEST_VALUE_MAX];
    char floor_version[FORGE_MANIFEST_VALUE_MAX];
    unsigned floor_revision = 0U;
    char safe[FORGE_PATH_MAX];
    char version_dir[FORGE_PATH_MAX];
    char resolve_tmp[FORGE_PATH_MAX];
    char marker[FORGE_PATH_MAX * 2U];
    char identity[FORGE_PATH_MAX * 2U];
    int query_latest;
    int min_given = min_version != NULL && min_version[0] != '\0';

    if (pin == NULL || reused == NULL) {
        forge_util_set_error(error, error_size, "registry materialize needs a pin");
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
    query_latest = 0;
    floor_version[0] = '\0';
    if (wanted_version != NULL && wanted_version[0] != '\0') {
        /* Exact pins name their bytes outright and bypass the baseline. */
        (void)snprintf(version, sizeof(version), "%s", wanted_version);
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
    if (version[0] != '\0') {
        forge_paths_safe_output_name(version, safe, sizeof(safe));
        if (lock_kind != NULL && strcmp(lock_kind, "git") == 0 &&
            lock_location != NULL && lock_location[0] != '\0') {
            char cache_hash[32];
            registry_cache_hash(lock_location, cache_hash, sizeof(cache_hash));
            if (strlen(package_dir) + strlen(safe) + strlen(cache_hash) + 2U >=
                sizeof(version_dir)) {
                forge_util_set_error(error, error_size, "registry cache path is too long");
                return -1;
            }
            {
                size_t base_length = strlen(package_dir);
                size_t name_length = strlen(safe);
                memcpy(version_dir, package_dir, base_length);
                version_dir[base_length] = '/';
                memcpy(version_dir + base_length + 1U, safe, name_length);
                version_dir[base_length + 1U + name_length] = '-';
                memcpy(version_dir + base_length + 2U + name_length,
                       cache_hash, strlen(cache_hash) + 1U);
            }
        } else {
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
            (void)snprintf(pin->version, sizeof(pin->version), "%s", locked.version);
            (void)snprintf(pin->kind, sizeof(pin->kind), "%s", locked.kind);
            (void)snprintf(pin->location, sizeof(pin->location), "%s", locked.location);
            (void)snprintf(pin->ref, sizeof(pin->ref), "%s", locked.ref);
            (void)snprintf(pin->commit, sizeof(pin->commit), "%s", locked.commit);
            (void)snprintf(pin->sha256, sizeof(pin->sha256), "%s", locked.sha256);
            pin->revision = locked.revision;
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
    if (forge_registry_query(logger, package, version, resolve_tmp, pin,
                             error, error_size) != 0) return -1;
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
        if (parse_recipe(body, base, pin, error, error_size) != 0) return -1;
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
                         ForgeRegistryPin *pin,
                         char *error, size_t error_size)
{
    char base[FORGE_PATH_MAX];
    char url[FORGE_PATH_MAX * 2U];
    char body[65536];
    char value[FORGE_PATH_MAX];
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
    if (strncmp(base, "file://", 7U) == 0) {
        /* Static layout: no query strings exist for files. */
        (void)logger;
        (void)tmp_path;
        return query_file_registry(base, package, version, pin,
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
    return 0;
}

