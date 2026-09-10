#ifndef FORGE_FETCH_H
#define FORGE_FETCH_H

#include <stddef.h>

#include "forge/log.h"

/*
 * Network downloads for registry dependencies, without adding a library
 * dependency: forge shells out to tools already on the host (curl,
 * PowerShell on Windows, tar), always via argv spawning — no shell is
 * ever consulted, so URLs and paths travel literally.
 */

/*
 * Transport policy for registry URLs (mirrors the git URL allowlist in
 * spirit: fail before any tool runs). https:// is always fine; http://
 * only for loopback hosts (tests); file:// only when
 * FORGE_ALLOW_UNSAFE_REGISTRY=1 is set (local registries and tests).
 * Option-shaped URLs are refused. Returns 0 when supported.
 */
int forge_fetch_url_is_supported(const char *url, char *error,
                                 size_t error_size);

/*
 * Downloads `url` to `dest_path` (truncated/created). The URL must already
 * have passed forge_fetch_url_is_supported. Returns 0 on success.
 */
int forge_fetch_to_file(ForgeLogger *logger, const char *url,
                        const char *dest_path, char *error, size_t error_size);

/*
 * Unpacks the .tar.gz `archive` into `dest_dir` (created with parents).
 * Returns 0 on success.
 */
int forge_fetch_unpack_tar_gz(ForgeLogger *logger, const char *archive,
                              const char *dest_dir, char *error,
                              size_t error_size);

#endif
