#ifndef FORGE_MANIFEST_H
#define FORGE_MANIFEST_H

#include <stddef.h>

#define FORGE_MANIFEST_MAX_ITEMS 128U
#define FORGE_MANIFEST_VALUE_MAX 256U
#define FORGE_MANIFEST_MAX_DEPS 64U

typedef struct ForgeStringList {
    char items[FORGE_MANIFEST_MAX_ITEMS][FORGE_MANIFEST_VALUE_MAX];
    size_t count;
} ForgeStringList;

/* One entry of [dependencies]: a path dependency, a git dependency
 * pinned by ref (tag, branch, or rev), or a registry dependency pinned
 * by version; the lockfile records the resolved commit / checksum. */
#define FORGE_DEP_FEATURES_MAX 8U
#define FORGE_FEATURE_NAME_MAX 32U
/* Canonical comma-joined feature sets never exceed 8 names of 32
 * characters; larger inputs fail before they reach a fixed buffer. */
#define FORGE_FEATURES_JOINED_MAX 512U
/* Canonical comma-joined feature sets never exceed 8 names of 32
 * characters; larger inputs fail before they reach a fixed buffer. */
#define FORGE_FEATURES_JOINED_MAX 512U
typedef struct ForgeDependency {
    char name[FORGE_MANIFEST_VALUE_MAX];
    char git_url[FORGE_MANIFEST_VALUE_MAX];
    char ref[FORGE_MANIFEST_VALUE_MAX];
    char path[FORGE_MANIFEST_VALUE_MAX];
    /* Registry deps only: package name on the registry and wanted version
     * ("" tracks the newest allowed state, like an unpinned git ref). */
    char registry[FORGE_MANIFEST_VALUE_MAX];
    char registry_version[FORGE_MANIFEST_VALUE_MAX];
    /* Registry deps only: minimum acceptable version, exclusive with the
     * exact pin above; "" means none. */
    char registry_min_version[FORGE_MANIFEST_VALUE_MAX];
    /* Registry deps only: requested feature names, validated against the
     * recipe after fetch; defaults apply unless default_features is 0. */
    char features[FORGE_DEP_FEATURES_MAX][FORGE_FEATURE_NAME_MAX + 1U];
    size_t feature_count;
    int default_features;
    /* Git deps only: clone/update git submodules alongside the checkout. */
    int submodules;
} ForgeDependency;

typedef struct ForgeDependencyList {
    ForgeDependency items[FORGE_MANIFEST_MAX_DEPS];
    size_t count;
} ForgeDependencyList;

typedef struct ForgeBuildProfile {
    int opt_level; /* -1 = compiler default; 0..3 = -O0..-O3 / /Od.. /O2 */
    int debug_info; /* 0 = none; >0 = debug info for -g / /Zi + /DEBUG */
    int warnings_as_errors; /* 0/1 */
    char std_version[FORGE_MANIFEST_VALUE_MAX]; /* "c11", "c++20", ...; "" = default */
    ForgeStringList cflags; /* raw extra flags, GCC/Clang dialect (translated for MSVC) */
} ForgeBuildProfile;

typedef struct ForgeManifest {
    char project_name[FORGE_MANIFEST_VALUE_MAX];
    /* "MAJOR.MINOR.PATCH" with optional "-prerelease"; empty when unset.
     * Shown in status lines and injected as FORGE_PROJECT_VERSION. */
    char project_version[FORGE_MANIFEST_VALUE_MAX];
    ForgeStringList c_source_dirs;
    ForgeStringList cpp_source_dirs;
    ForgeStringList asm_source_dirs;
    ForgeStringList target_os;
    ForgeStringList target_arch;
    char compiler_override[FORGE_MANIFEST_VALUE_MAX];
    ForgeDependencyList dependencies;
    ForgeBuildProfile debug_profile;
    ForgeBuildProfile release_profile;
} ForgeManifest;

/* Loads the supported Forge.toml subset. Returns 0 on success. */
int forge_manifest_load(const char *path, ForgeManifest *manifest,
                        char *error, size_t error_size);

/* Feature names for registry dependencies: 1-32 of letters, digits, '-',
 * '_' (no dots). Returns 1 when valid, 0 otherwise. */
int forge_feature_name_is_valid(const char *name);

/*
 * Splits a comma-separated feature list ("ssl, http") into dependency
 * slots in canonical (sorted, deduplicated) order. Used by the manifest
 * parser, the lockfile reader, and `forge add` so every spelling
 * normalizes identically. `name` names the dependent for diagnostics.
 * Returns 0 on success.
 */
int forge_parse_feature_list(const char *name, const char *text,
                             ForgeDependency *dependency,
                             char *error, size_t error_size);
/*
 * Total semver precedence for validated versions: numeric MAJOR/MINOR/PATCH,
 * releases outranking pre-releases, pre-release identifiers per semver 11.4;
 * "+build" suffixes are ignored. Returns -1 when a < b, 0 when equal,
 * 1 when a > b.
 */
int forge_version_compare(const char *a, const char *b);

#endif
