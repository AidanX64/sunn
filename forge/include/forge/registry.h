#ifndef FORGE_REGISTRY_H
#define FORGE_REGISTRY_H

#include <stddef.h>

#include "forge/compiler.h"
#include "forge/log.h"
#include "forge/manifest.h"
#include "forge/paths.h"
#include "forge/sha256.h"

#define FORGE_REGISTRY_MAX_PATCHES 32U

/* A registry recipe describes upstream source; it is not an artifact host. */

typedef struct ForgeRegistryPin {
    char version[FORGE_MANIFEST_VALUE_MAX];
    char kind[8];
    char location[FORGE_PATH_MAX];
    char ref[FORGE_MANIFEST_VALUE_MAX];
    char commit[FORGE_MANIFEST_VALUE_MAX];
    char sha256[FORGE_SHA256_HEX_LENGTH + 1U];
    size_t patch_count;
    char patches[FORGE_REGISTRY_MAX_PATCHES][FORGE_PATH_MAX];
} ForgeRegistryPin;

/*
 * Reads FORGE_REGISTRY_URL (required) into `base_out` with any trailing
 * '/' stripped. Empty/unset names the variable in `error`. Returns 0.
 */
int forge_registry_base_url(char *base_out, size_t base_size,
                            char *error, size_t error_size);

/*
 * Queries the resolve endpoint for `package` (`version` "" means latest)
 * and fills `pin` (resolved version, source recipe, and patch names).
 * `tmp_path` is scratch space for the response body (overwritten).
 * Returns 0.
 */
int forge_registry_query(ForgeLogger *logger, const char *package,
                         const char *version, const char *tmp_path,
                         ForgeRegistryPin *pin,
                         char *error, size_t error_size);

/*
 * Ensures `package_dir/<version>` holds the verified, unpacked dependency.
 * `dep_name` is the local [dependencies] name (used in user-facing errors);
 * `package` the registry package. `wanted_version` "" keeps the lock pin
 * (offline-friendly) unless `force_update` asks for the newest allowed
 * state. Sets `*reused` to 1
 * when a matching checkout was already on disk (no network touched), 0
 * after a fetch. `root_out` receives the unpacked directory, `pin` the pin
 * to record. --offline only ever reuses; anything needing network fails
 * naming the dependency. Returns 0 on success.
 */
int forge_registry_materialize(ForgeLogger *logger, const char *dep_name,
                               const char *package,
                               const char *wanted_version,
                               const char *lock_version, const char *lock_kind,
                               const char *lock_location,
                               const char *lock_ref, const char *lock_commit,
                               const char *lock_sha256,
                               int force_update, int offline,
                               const char *package_dir,
                               char *root_out, size_t root_size,
                               ForgeRegistryPin *pin, int *reused,
                               char *error, size_t error_size);

#endif
