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

/* Maximum versions consulted when picking the newest entry satisfying
 * a version-range (bounded so a hostile index cannot force unbounded
 * queries; indexes themselves cap at 1000 in lib/sunn-registry.ts). */
#define FORGE_REGISTRY_MAX_LISTED_VERSIONS 256U

/* Overlay search path: colon-separated on POSIX, semicolon-separated on
 * Windows (FORGE_OVERLAYS), plus per-invocation --overlay dirs. Entries
 * point at site roots with the same static layout as file:// registries
 * (packages/sunn.registry.json + packages/<name>/<ver>.json). */
#define FORGE_REGISTRY_MAX_OVERLAYS 16U

/* Recipe revisions share the registry schema's bound (lib/sunn-registry.ts). */
#define FORGE_REGISTRY_MAX_REVISION 1000000U

/* Optional recipe features (vcpkg-style): named build variants carrying
 * extra compiler flags and extra transitive dependencies. Bounds mirror
 * the registry schema; definitions live in the recipe JSON, selection in
 * the consumer manifest. */
#define FORGE_RECIPE_FEATURES_MAX 8U
#define FORGE_FEATURE_CFLAG_MAX 8U
#define FORGE_FEATURE_DEPS_MAX 4U

typedef struct ForgeFeatureDep {
    char package[FORGE_MANIFEST_VALUE_MAX];
    char version[FORGE_MANIFEST_VALUE_MAX];
    char min_version[FORGE_MANIFEST_VALUE_MAX];
    char range[FORGE_VERSION_RANGE_MAX];
    char max_version[FORGE_MANIFEST_VALUE_MAX];
} ForgeFeatureDep;

typedef struct ForgeFeatureDef {
    char name[FORGE_FEATURE_NAME_MAX + 1U];
    char cflags[FORGE_FEATURE_CFLAG_MAX][FORGE_MANIFEST_VALUE_MAX];
    size_t cflag_count;
    ForgeFeatureDep deps[FORGE_FEATURE_DEPS_MAX];
    size_t dep_count;
} ForgeFeatureDef;

typedef struct ForgeFeatureDefs {
    ForgeFeatureDef items[FORGE_RECIPE_FEATURES_MAX];
    size_t count;
    char defaults[FORGE_DEP_FEATURES_MAX][FORGE_FEATURE_NAME_MAX + 1U];
    size_t default_count;
} ForgeFeatureDefs;

typedef struct ForgeRegistryPin {
    char version[FORGE_MANIFEST_VALUE_MAX];
    /* Recipe revision: a recipe fix without a new upstream release.
     * Absent in older recipes and lockfiles, where it means 0. */
    unsigned revision;
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
 * and fills `pin` (resolved version, source recipe, and patch names) plus
 * `defs` (the recipe's feature definitions, zeroed first). `tmp_path` is
 * scratch space for the response body (overwritten). Returns 0.
 */
int forge_registry_query(ForgeLogger *logger, const char *package,
                         const char *version, const char *tmp_path,
                         ForgeRegistryPin *pin, ForgeFeatureDefs *defs,
                         char *error, size_t error_size);

/*
 * Reads the recipe's feature definitions ("features" array plus the
 * "default-features" names) into `defs`, zeroed first. Absent sections
 * mean "no features". Malformed shapes, bad names, over-cap counts, bad
 * versions, or defaults naming undefined features fail loudly.
 * Returns 0 on success.
 */
int forge_registry_parse_features(const char *body, ForgeFeatureDefs *defs,
                                  char *error, size_t error_size);

/* Canonical comma-joined form for feature name sets (sorted, deduplicated):
 * the stable spelling for cache suffixes, lock pins, and identity strings.
 * Joined output never exceeds 8 names of 32 characters; larger inputs fail.
 * Returns 0 on success. */
int forge_features_join(size_t count,
                        const char names[][FORGE_FEATURE_NAME_MAX + 1U],
                        char *out, size_t out_size);

/*
 * Expands one declaration into its effective set: recipe defaults (unless
 * disabled) plus the declared request, validated against the recipe —
 * unknown names fail naming the available features. Writes the canonical
 * joined form. Returns 0 on success.
 */
int forge_features_effective(const ForgeFeatureDefs *defs,
                             const char declared[][FORGE_FEATURE_NAME_MAX + 1U],
                             size_t ndeclared, int use_defaults,
                             const char *dep_name,
                             char *out, size_t out_size,
                             char *error, size_t error_size);

/*
 * Ensures `package_dir/<version>` holds the verified, unpacked dependency.
 * `dep_name` is the local [dependencies] name (used in user-facing errors);
 * `package` the registry package. `wanted_version` names an exact pin (""
 * when the entry floats); `min_version` names a manifest minimum ("" when
 * the entry is exact or bare); `range` names a version-range requirement
 * ("" when none, exclusive with exact/minimum); `max_version` names an
 * inclusive upper bound ("" when none, never with exact). `declared_features` carries the canonical
 * comma-joined requested features ("" when none) with `use_defaults`
 * saying whether recipe defaults apply; the cache directory separates on
 * that spelling while `defs` receives the recipe's feature definitions
 * for validation and expansion downstream. `lock_*` carry the Forge.lock pin
 * (`lock_revision` 0 when the lock predates revisions); `lock_features` is
 * the lock's recorded effective set ("" when none) so a cache hit can tell
 * whether definitions are still needed for validation. Exact pins bypass
 * the registry baseline; minimums and bare entries resolve no lower, and
 * `force_update` tracks newest (minimum-checked). Sets `*reused` to 1
 * when a matching checkout was already on disk (no network touched), 0
 * after a fetch. `root_out` receives the unpacked directory, `pin` the pin
 * to record. --offline only ever reuses; anything needing network fails
 * naming the dependency. Returns 0 on success.
 */
int forge_registry_materialize(ForgeLogger *logger, const char *dep_name,
                               const char *package,
                               const char *wanted_version,
                               const char *min_version,
                               const char *range,
                               const char *max_version,
                               const char *declared_features,
                               int use_defaults,
                               const char *lock_version, const char *lock_kind,
                               const char *lock_location,
                               const char *lock_ref, const char *lock_commit,
                               const char *lock_sha256,
                               unsigned lock_revision,
                               const char *lock_features,
                               int force_update, int offline,
                               const char *package_dir,
                               char *root_out, size_t root_size,
                               ForgeRegistryPin *pin, ForgeFeatureDefs *defs,
                               int *reused,
                               char *error, size_t error_size);

#endif
