#ifndef FORGE_LOG_H
#define FORGE_LOG_H

#include <stddef.h>
#include <stdio.h>

#define FORGE_LOG_PATH_MAX 1024U

typedef struct ForgeLogger {
    FILE *file;
    char path[FORGE_LOG_PATH_MAX];
} ForgeLogger;

/*
 * Console verbosity, set once per invocation from the CLI (-q/-v/-vv). It only
 * filters what reaches the terminal; the target/logs file always receives the
 * full stream so a failed build stays greppable after the fact.
 */
typedef enum ForgeVerbosity {
    FORGE_VERBOSITY_QUIET = 0,       /* errors and program output only */
    FORGE_VERBOSITY_NORMAL = 1,      /* cargo-style milestones */
    FORGE_VERBOSITY_VERBOSE = 2,     /* adds per-file compile/link lines */
    FORGE_VERBOSITY_VERY_VERBOSE = 3 /* adds full command echoes too */
} ForgeVerbosity;

/* How important one log line is on the terminal. */
typedef enum ForgeConsoleLevel {
    /* Per-file progress ("up-to-date: src/main.c"): -v and above. */
    FORGE_CONSOLE_PROGRESS = 0,
    /* Full compiler/linker command echoes: -vv only. */
    FORGE_CONSOLE_COMMAND = 1
} ForgeConsoleLevel;

void forge_log_set_verbosity(ForgeVerbosity verbosity);
ForgeVerbosity forge_log_get_verbosity(void);

/*
 * Milestone logger for the current invocation (Compiling/Finished/Running).
 * Mirrors forge_build_set_logger: set alongside it when a session starts so
 * status lines can also land in the invocation log file.
 */
void forge_log_set_session_logger(ForgeLogger *logger);

/*
 * Prints one cargo-style milestone line ("   Compiling hello v0.1.0") to the
 * terminal — label right-aligned into 12 columns, green when colors are on,
 * suppressed entirely at QUIET — and appends "[INFO] [status]" to the
 * session log file when one is attached.
 */
void forge_log_status(const char *label, const char *format, ...);

/* Monotonic seconds for the "Finished ... in <t>s" timing. */
double forge_log_monotonic_seconds(void);

/*
 * Creates <project_root>/target/logs/<kind>-<timestamp>.log. The logs directory
 * is placed inside the project's own target directory so output is anchored to
 * the manifest location rather than the invoking process's working directory.
 */
int forge_logger_init_in(ForgeLogger *logger, const char *project_root,
                         const char *kind, char *error, size_t error_size);

void forge_logger_close(ForgeLogger *logger);
void forge_logger_log(ForgeLogger *logger, const char *stage,
                      const char *format, ...);
void forge_logger_error(ForgeLogger *logger, const char *stage,
                        const char *format, ...);
/*
 * Detail line: always written to the log file, echoed to the terminal only at
 * VERBOSE and above (per-file progress, dispatch notes, stage headers).
 */
void forge_logger_detail(ForgeLogger *logger, const char *stage,
                         const char *format, ...);
/*
 * Command echo: always written to the log file, terminal only at VERY_VERBOSE.
 */
void forge_logger_command(ForgeLogger *logger, const char *stage,
                          const char *format, ...);

#endif
