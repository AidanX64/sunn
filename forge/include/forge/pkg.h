#ifndef FORGE_PKG_H
#define FORGE_PKG_H

#include <stddef.h>

#include "forge/log.h"

/*
 * Dependency management verbs that edit the manifest's [dependencies]
 * section in place (Cargo-style `forge add` / `forge remove`).
 *
 * Both operations refuse to touch a manifest that does not parse, leave the
 * rest of the file byte-for-byte identical, and finish by re-resolving
 * [dependencies] so Forge.lock gains (or loses) the matching pin. When an
 * added dependency cannot be resolved (bad URL, missing path), the manifest
 * edit is rolled back before returning an error.
 */

/*
 * Adds `name` backed by a git source (`git_url`, optionally pinned by
 * `ref_value` under the field named by `ref_kind`: "tag", "branch", or
 * "rev"), a path source (`dep_path`), or a registry source
 * (`registry_package`, pinned exactly by `registry_version` or floored by
 * `registry_min_version`; "" for both tracks the baseline, newest when the
 * registry states none — the resolved release is still pinned in
 * Forge.lock immediately). At most one of `registry_version` /
 * `registry_min_version` may be non-empty; `registry_features` is a
 * comma-separated feature request ("" for none) and
 * `registry_no_default_features` disables recipe defaults — both need a
 * registry source. Exactly one of `git_url` / `dep_path` /
 * `registry_package` must be non-empty. Resolution progress goes to
 * `logger`.
 * Returns 0 on success.
 */
int forge_pkg_add(const char *manifest_path, const char *name,
                  const char *git_url, const char *ref_kind,
                  const char *ref_value, const char *dep_path,
                  const char *registry_package, const char *registry_version,
                  const char *registry_min_version,
                  const char *registry_features,
                  int registry_no_default_features,
                  ForgeLogger *logger, char *error, size_t error_size);

/* Removes `name` from [dependencies]. Unknown names are an error naming the
 * dependencies that do exist. Returns 0 on success. */
int forge_pkg_remove(const char *manifest_path, const char *name,
                     ForgeLogger *logger, char *error, size_t error_size);

#endif
