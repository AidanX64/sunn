#ifndef FORGE_PATHS_H
#define FORGE_PATHS_H

#include <stddef.h>

/* Canonical path buffer size used across Forge. */
#define FORGE_PATH_MAX 1024U

/* Joins `left` + "/" + `right` into `destination`; -1 if it cannot fit. */
int forge_paths_join(char *destination, size_t destination_size,
                     const char *left, const char *right);

/* Resolves a manifest-relative path against the project root. Absolute paths
 * pass through unchanged; relative paths are anchored to `root`. Returns 0 on
 * success. */
int forge_paths_resolve(const char *root, const char *relative,
                        char *destination, size_t destination_size);

/* Turns `path` into a fully normalized absolute path (resolving ".", "..",
 * and separator style). Paths that cannot be resolved (e.g. they do not
 * exist yet, on POSIX) fall back to their raw text. Returns 0 unless the
 * result would not fit. */
int forge_paths_absolute(const char *path, char *destination,
                         size_t destination_size);

/* Creates `path` and all missing parents (like mkdir -p). Returns 0 on
 * success, -1 with a message in `error` otherwise. */
int forge_paths_ensure_directory(const char *path, char *error, size_t error_size);

/* Removes the directory tree at `path` recursively. A missing root is not an
 * error. Returns 0 on success, -1 with a message in `error` otherwise. */
int forge_paths_remove_tree(const char *path, char *error, size_t error_size);

/* Computes the absolute directory containing `manifest_path`. Returns 0 on
 * success with the result in `root`. */
int forge_paths_project_root(const char *manifest_path, char *root,
                             size_t root_size, char *error, size_t error_size);

/*
 * Searches the current working directory and each parent for `manifest_name`.
 * Returns 1 with the path in `found` when found, 0 when no ancestor contains
 * it, and -1 when the search itself failed (buffer limits, OS error).
 */
int forge_paths_find_manifest(const char *manifest_name, char *found,
                              size_t found_size);

/* Copies the current working directory into `buffer`. Returns 0 on success. */
int forge_paths_current_directory(char *buffer, size_t buffer_size);

/* Maps `project_name` to a portable executable base name: all characters that
 * are not alphanumeric, '-', '_', or '.' become '-'. */
void forge_paths_safe_output_name(const char *project_name, char *output,
                                  size_t output_size);

#endif