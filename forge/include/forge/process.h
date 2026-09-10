#ifndef FORGE_PROCESS_H
#define FORGE_PROCESS_H

#include <stddef.h>

/*
 * Runs a program directly (no shell) from a NULL-terminated argv array. When
 * `redirect_to` is non-NULL the child's stdout and stderr are written to that
 * path instead of the console (append when appending is non-zero, truncate
 * otherwise). The child's exit status is stored in `*exit_code`; returns 0 on
 * success (including a non-zero child exit) and -1 only when the process could
 * not be created at all.
 */
int forge_process_run(char *const *argv, const char *redirect_to,
                      int appending, int *exit_code, char *error, size_t error_size);

#endif