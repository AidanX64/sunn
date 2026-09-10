#ifndef FORGE_FINGERPRINT_H
#define FORGE_FINGERPRINT_H

#include <stddef.h>

/*
 * Freshness checks for incremental builds. All comparisons use the full
 * timestamp precision the platform provides (100 ns FILETIME on Windows,
 * nanosecond stat fields where the libc exposes them), so a quick
 * edit-then-rebuild within the same second is never missed.
 */

/* Returns 1 when `path`'s mtime is strictly newer than `reference`'s.
 * A missing file counts as old; a missing reference yields 0. */
int forge_fingerprint_path_is_newer(const char *path, const char *reference);

/* Writes the modification time of `path` as "<seconds>.<nanoseconds>", or
 * "missing" when the path does not exist, so callers can record and later
 * compare exact input states (link-input stamps). Returns 0 on success. */
int forge_fingerprint_mtime_text(const char *path, char *text, size_t text_size);

#include "forge/paths.h"

/*
 * Returns 1 when the object file is at least as new as the source, was built
 * from the exact command described by `command_hash` (see below), and — when
 * `track_headers` is set — is at least as new as every header the compiler
 * recorded via -MMD -MF; 0 when any of that fails. Toolchains with no
 * dependency file (MSVC, assembly sources) compare sources only.
 */
int forge_fingerprint_object_fresh(const char *object_path, const char *source_path,
                                   int track_headers, unsigned int command_hash);

/* Stable FNV-1a hash of a text blob. Callers hash the finalized compile
 * command line so a changed flag set — edited profile cflags, a bumped
 * project version — recompiles exactly the objects that would differ. */
unsigned int forge_fingerprint_hash_text(const char *text);

/*
 * Records `command_hash` in a "<object>.cmdhash" sidecar after a successful
 * compile. Best effort: a failure simply makes the next run recompile once.
 */
void forge_fingerprint_record_command(const char *object_path,
                                      unsigned int command_hash);

/*
 * Picks a collision-free object path under `object_directory` for `source_path`.
 * Objects are named after a stable hash of the source path; if two sources hash
 * identically a numeric suffix is appended until the name is unique against the
 * already-claimed `used` names. Returns 0 on success.
 */
int forge_fingerprint_object_name(const char *object_directory, const char *source_path,
                                  char (*used)[FORGE_PATH_MAX], size_t used_count,
                                  char *object_path, size_t object_path_size);

#endif