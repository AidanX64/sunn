#ifndef FORGE_REGISTRY_H
#define FORGE_REGISTRY_H

#include <stddef.h>

#include "forge/compiler.h"
#include "forge/log.h"
#include "forge/manifest.h"
#include "forge/paths.h"
#include "forge/sha256.h"

/*
 * Client for the sunn native registry (Forge.toml-era: static JSON over
 * https, e.g. GET {base}/api/forge/v1/resolve?name=&version=&triplet=).
 * The registry only ever yields a tarball URL + sha256; every trust,
 * caching, lockfile, and build decision stays in deps.c, exactly like
 * a git source that happens to arrive as an archive.
 */

typedef struct ForgeRegistryPin {
    char version[FORGE_MANIFEST_VALUE_MAX];
    char sha256[FORGE_SHA256_HEX_LENGTH + 1U];
    char url[FORGE_PATH_MAX];
} ForgeRegistryPin;

/*
 * Reads FORGE_REGISTRY_URL (required) into `base_out` with any trailing
 * '/' stripped. Empty/unset names the variable in `error`. Returns 0.
 */
int forge_registry_base_url(char *base_out, size_t base_size,
                            char *error, size_t error_size);

/* Host triplet the resolve endpoint understands ("x64-windows", ...). */
void forge_registry_host_triplet(char *triplet_out, size_t triplet_size);

/*
 * Queries the resolve endpoint for `package` (`version` "" means latest)
 * and fills `pin` (resolved version, absolute tarball URL, sha256).
 * `tmp_path` is scratch space for the response body (overwritten).
 * `triplet` selects the per-triplet artifact; a registry without a build
 * for it is a clean error naming package and triplet. Returns 0.
 */
int forge_registry_query(ForgeLogger *logger, const char *package,
                         const char *version, const char *triplet,
                         const char *tmp_path, ForgeRegistryPin *pin,
                         char *error, size_t error_size);

/*
 * Ensures `package_dir/<version>` holds the verified, unpacked dependency.
 * `dep_name` is the local [dependencies] name (used in user-facing errors);
 * `package` the registry package. `wanted_version` "" keeps the lock pin
 * (offline-friendly) unless `force_update` asks for the newest allowed
 * state; `lock_version` / `lock_sha` "" mean unpinned. Sets `*reused` to 1
 * when a matching checkout was already on disk (no network touched), 0
 * after a fetch. `root_out` receives the unpacked directory, `pin` the pin
 * to record. --offline only ever reuses; anything needing network fails
 * naming the dependency. Returns 0 on success.
 */
int forge_registry_materialize(ForgeLogger *logger, const char *dep_name,
                               const char *package,
                               const char *wanted_version,
                               const char *lock_version, const char *lock_sha,
                               const char *lock_url,
                               int force_update, int offline,
                               const char *package_dir,
                               char *root_out, size_t root_size,
                               ForgeRegistryPin *pin, int *reused,
                               char *error, size_t error_size);

#endif
