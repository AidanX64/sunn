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

/*
 * Same as forge_process_run, but the child starts in `work_dir` (NULL keeps
 * the current directory). Needed when an argument must stay relative: some
 * child tools misread absolute Windows paths (GNU tar parses "C:/..." as a
 * remote "host:file"), so callers chdir the child next to the file and hand
 * it the bare name instead.
 */
int forge_process_run_at(const char *work_dir, char *const *argv,
                         const char *redirect_to, int appending,
                         int *exit_code, char *error, size_t error_size);

#endif