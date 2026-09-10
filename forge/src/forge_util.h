#ifndef FORGE_UTIL_H
#define FORGE_UTIL_H

#include <stddef.h>
#include <stdio.h>

/* Fills `error` (if there is room) with a formatted message. */
void forge_util_set_error(char *error, size_t error_size, const char *format, ...);

/* Strips leading and trailing whitespace from *text in place, returning text. */
char *forge_util_trim(char *text);

/* Returns 1 if `text` ends with `suffix`. */
int forge_util_has_suffix(const char *text, const char *suffix);

/*
 * Returns 1 if `program` is invocable from PATH, 0 otherwise. Uses a real PATH
 * sweep rather than a shell so no command interpreter is ever consulted.
 */
int forge_util_program_available(const char *program);

/*
 * Writes `final_path` atomically: `write_body` fills a temporary file next to
 * the final one, and only after it closes cleanly does the temporary swap into
 * place (rename / MoveFileEx-with-replace). Readers therefore never see a
 * truncated or half-written file, and a crash leaves the previous contents
 * intact. `write_body` returns nonzero to abort the replacement. Returns 0 on
 * success, -1 with a human-readable message in `error`.
 */
int forge_util_replace_file(const char *final_path,
                            int (*write_body)(void *user_data, FILE *file),
                            void *user_data,
                            char *error, size_t error_size);

#endif