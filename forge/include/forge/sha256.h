#ifndef FORGE_SHA256_H
#define FORGE_SHA256_H

#include <stddef.h>

/* SHA-256 hex digest length without the terminator. */
#define FORGE_SHA256_HEX_LENGTH 64U

/*
 * Self-contained SHA-256 (no third-party code, no OS crypto APIs) so
 * registry tarballs can be verified on every host forge builds on.
 * Follows the surrounding codebase: explicit sizes, no heap, -1 with a
 * human-readable message in `error` on failure.
 */

/* Hashes `length` bytes at `data`; `hex_out` needs 65 bytes. */
void forge_sha256_buffer(const unsigned char *data, size_t length,
                         char *hex_out);

/*
 * Hashes the file at `path` in streaming fashion; `hex_out` needs
 * 65 bytes. Returns 0 on success.
 */
int forge_sha256_file(const char *path, char *hex_out,
                      char *error, size_t error_size);

#endif
