#ifdef _MSC_VER
#define _CRT_SECURE_NO_WARNINGS
#endif

#include "forge/platform.h"

/*
 * Nanosecond clocks and terminal detection need POSIX visibility; requesting
 * it here (rather than relying on the build flags) keeps this file's console
 * and timing helpers self-contained.
 *
 * This must precede every libc include: glibc snapshots feature-test macros
 * when its first header loads, so a later definition is silently ignored and
 * -std=c2x strict mode would hide clock_gettime/CLOCK_MONOTONIC and fileno.
 * Apple is excluded on purpose: strict POSIX there hides BSD extensions, and
 * Darwin declares everything this file needs by default.
 */
#if !FORGE_PLATFORM_WINDOWS && !defined(__APPLE__)
#define _POSIX_C_SOURCE 200809L
#endif

#include <errno.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#if FORGE_PLATFORM_WINDOWS
#define WIN32_LEAN_AND_MEAN
#if defined(_MSC_VER) || defined(__MINGW32__) || defined(__MINGW64__)
#define FORGE_LOG_USE_FSOPEN 1
#include <share.h>
#endif
#include <io.h>
#include <windows.h>
#else
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>
#endif

#include "forge/log.h"
#include "forge_util.h"

#define FORGE_LOG_MESSAGE_MAX 2048U

/* Console verbosity: written once by the CLI before any worker starts, so no
 * lock is needed to read it from compile threads afterwards. */
static ForgeVerbosity active_verbosity = FORGE_VERBOSITY_NORMAL;

/* Attached by commands.c so milestone lines also land in target/logs. */
static ForgeLogger *session_logger;

/* Tri-state (-1 unset) so detection runs at most once, lazily. */
static int colors_enabled = -1;

static int create_directory(const char *path, char *error, size_t error_size)
{
#if FORGE_PLATFORM_WINDOWS
    if (CreateDirectoryA(path, NULL) == 0 &&
        GetLastError() != ERROR_ALREADY_EXISTS) {
        forge_util_set_error(error, error_size, "could not create output directory '%s' (error %lu)",
                  path, (unsigned long)GetLastError());
        return -1;
    }
#else
    if (mkdir(path, 0777) != 0 && errno != EEXIST) {
        forge_util_set_error(error, error_size, "could not create output directory '%s': %s",
                  path, strerror(errno));
        return -1;
    }
#endif
    return 0;
}

static int create_log_directory(const char *root, char *error, size_t error_size)
{
    char path[FORGE_LOG_PATH_MAX];

    if ((size_t)snprintf(path, sizeof(path), "%s/target", root) >= sizeof(path)) {
        forge_util_set_error(error, error_size, "project root path is too long");
        return -1;
    }
    if (create_directory(path, error, error_size) != 0) {
        return -1;
    }
    if ((size_t)snprintf(path, sizeof(path), "%s/target/logs", root) >= sizeof(path)) {
        forge_util_set_error(error, error_size, "project root path is too long");
        return -1;
    }
    return create_directory(path, error, error_size);
}

static FILE *open_log_for_write(const char *path)
{
#if defined(FORGE_LOG_USE_FSOPEN)
    return _fsopen(path, "w", _SH_DENYNO);
#else
    return fopen(path, "w");
#endif
}

static void timestamp(char *value, size_t value_size)
{
#if FORGE_PLATFORM_WINDOWS
    SYSTEMTIME now;
    GetLocalTime(&now);
    (void)snprintf(value, value_size, "%04u%02u%02u-%02u%02u%02u-%03u",
                   (unsigned int)now.wYear, (unsigned int)now.wMonth,
                   (unsigned int)now.wDay, (unsigned int)now.wHour,
                   (unsigned int)now.wMinute, (unsigned int)now.wSecond,
                   (unsigned int)now.wMilliseconds);
#else
    time_t now = time(NULL);
    struct tm *local = localtime(&now);
    if (local == NULL) {
        (void)snprintf(value, value_size, "unknown-time");
        return;
    }
    (void)strftime(value, value_size, "%Y%m%d-%H%M%S", local);
#endif
}

int forge_logger_init_in(ForgeLogger *logger, const char *project_root,
                         const char *kind, char *error, size_t error_size)
{
    char stamp[64];
    unsigned int suffix;

    if (logger == NULL || kind == NULL || kind[0] == '\0' ||
        project_root == NULL || project_root[0] == '\0') {
        forge_util_set_error(error, error_size, "logger, project root, and log kind are required");
        return -1;
    }
    *logger = (ForgeLogger){0};
    if (create_log_directory(project_root, error, error_size) != 0) {
        return -1;
    }
    timestamp(stamp, sizeof(stamp));
    for (suffix = 0U; suffix < 1000U; ++suffix) {
        int written;
        if (suffix == 0U) {
            written = snprintf(logger->path, sizeof(logger->path),
                               "%s/target/logs/%s-%s.log", project_root, kind, stamp);
        } else {
            written = snprintf(logger->path, sizeof(logger->path),
                               "%s/target/logs/%s-%s-%u.log",
                               project_root, kind, stamp, suffix);
        }
        if (written < 0 || (size_t)written >= sizeof(logger->path)) {
            forge_util_set_error(error, error_size, "log path is too long");
            return -1;
        }
        logger->file = fopen(logger->path, "r");
        if (logger->file == NULL) {
            logger->file = open_log_for_write(logger->path);
            if (logger->file == NULL) {
                forge_util_set_error(error, error_size, "could not create log '%s': %s",
                          logger->path, strerror(errno));
                return -1;
            }
            return 0;
        }
        (void)fclose(logger->file);
        logger->file = NULL;
    }
    forge_util_set_error(error, error_size, "could not reserve a unique log file name");
    return -1;
}

void forge_log_set_verbosity(ForgeVerbosity verbosity)
{
    if (verbosity >= FORGE_VERBOSITY_QUIET && verbosity <= FORGE_VERBOSITY_VERY_VERBOSE) {
        active_verbosity = verbosity;
    }
}

ForgeVerbosity forge_log_get_verbosity(void)
{
    return active_verbosity;
}

void forge_log_set_session_logger(ForgeLogger *logger)
{
    session_logger = logger;
}

double forge_log_monotonic_seconds(void)
{
#if FORGE_PLATFORM_WINDOWS
    LARGE_INTEGER frequency;
    LARGE_INTEGER counter;

    if (QueryPerformanceFrequency(&frequency) == 0 || frequency.QuadPart == 0 ||
        QueryPerformanceCounter(&counter) == 0) {
        return 0.0;
    }
    return (double)counter.QuadPart / (double)frequency.QuadPart;
#else
    struct timespec now;

    if (clock_gettime(CLOCK_MONOTONIC, &now) != 0) {
        return 0.0;
    }
    return (double)now.tv_sec + (double)now.tv_nsec / 1000000000.0;
#endif
}

/*
 * Decides once whether status labels may be colored: NO_COLOR wins, a piped
 * stdout stays plain, Windows consoles only get ANSI after enabling virtual
 * terminal processing (Windows 10+), and every other tty is assumed to
 * understand the escape codes.
 */
static int console_colors_enabled(void)
{
    if (colors_enabled >= 0) {
        return colors_enabled;
    }
    colors_enabled = 0;
    {
        const char *no_color = getenv("NO_COLOR");

        if (no_color == NULL || no_color[0] == '\0') {
#if FORGE_PLATFORM_WINDOWS
            HANDLE console = GetStdHandle(STD_OUTPUT_HANDLE);
            DWORD mode = 0;

            if (_isatty(_fileno(stdout)) && console != INVALID_HANDLE_VALUE &&
                GetConsoleMode(console, &mode) != 0) {
                if (SetConsoleMode(console,
                                   mode | ENABLE_VIRTUAL_TERMINAL_PROCESSING) != 0) {
                    colors_enabled = 1;
                }
            }
#else
            colors_enabled = isatty(fileno(stdout));
#endif
        }
    }
    return colors_enabled;
}

#define FORGE_STATUS_LABEL_WIDTH 12U
#define FORGE_COLOR_BOLD_GREEN "\x1b[1;32m"
#define FORGE_COLOR_RESET "\x1b[0m"

void forge_log_status(const char *label, const char *format, ...)
{
    va_list arguments;
    char message[FORGE_LOG_MESSAGE_MAX];
    char padded[FORGE_STATUS_LABEL_WIDTH + 1U];
    size_t label_length;
    int colored;

    if (label == NULL || format == NULL) {
        return;
    }
    va_start(arguments, format);
    (void)vsnprintf(message, sizeof(message), format, arguments);
    va_end(arguments);

    /* QUIET shows errors and program output only; milestones disappear. */
    if (active_verbosity != FORGE_VERBOSITY_QUIET) {
        label_length = strlen(label);
        if (label_length > FORGE_STATUS_LABEL_WIDTH) {
            label_length = FORGE_STATUS_LABEL_WIDTH;
        }
        memset(padded, ' ', FORGE_STATUS_LABEL_WIDTH - label_length);
        memcpy(padded + FORGE_STATUS_LABEL_WIDTH - label_length, label, label_length);
        padded[FORGE_STATUS_LABEL_WIDTH] = '\0';
        colored = console_colors_enabled();
        if (colored) {
            (void)fputs(FORGE_COLOR_BOLD_GREEN, stdout);
        }
        (void)fprintf(stdout, "%s ", padded);
        if (colored) {
            (void)fputs(FORGE_COLOR_RESET, stdout);
        }
        (void)fprintf(stdout, "%s\n", message);
        (void)fflush(stdout);
    }

    /* Milestones join everything else in the invocation log. */
    if (session_logger != NULL && session_logger->file != NULL) {
        (void)fprintf(session_logger->file, "[INFO] [status] %s %s\n", label, message);
        (void)fflush(session_logger->file);
    }
}

static void write_log(ForgeLogger *logger, FILE *stream, const char *level,
                      const char *stage, const char *format, va_list arguments)
{
    va_list copy;

    (void)fprintf(stream, "[%s] [%s] ", level, stage);
    va_copy(copy, arguments);
    (void)vfprintf(stream, format, copy);
    va_end(copy);
    fputc('\n', stream);
    (void)fflush(stream);

    if (logger != NULL && logger->file != NULL) {
        (void)fprintf(logger->file, "[%s] [%s] ", level, stage);
        (void)vfprintf(logger->file, format, arguments);
        fputc('\n', logger->file);
        (void)fflush(logger->file);
    }
}

void forge_logger_log(ForgeLogger *logger, const char *stage,
                      const char *format, ...)
{
    va_list arguments;
    va_start(arguments, format);
    write_log(logger, stdout, "INFO", stage, format, arguments);
    va_end(arguments);
}

/*
 * Shared backend of the classified emitters: the log file always receives the
 * line; the terminal only sees it when the chosen verbosity allows.
 */
static void write_classified(ForgeLogger *logger, const char *stage,
                             ForgeConsoleLevel level, const char *format,
                             va_list arguments)
{
    ForgeVerbosity required = level == FORGE_CONSOLE_COMMAND
                                  ? FORGE_VERBOSITY_VERY_VERBOSE
                                  : FORGE_VERBOSITY_VERBOSE;

    /*
     * Copy before the first consumption: a va_list becomes indeterminate once
     * vfprintf has walked it (C11 7.16), so the file branch uses its own copy
     * and `arguments` stays intact for the terminal echo below.
     */
    va_list file_arguments;

    va_copy(file_arguments, arguments);
    if (logger != NULL && logger->file != NULL) {
        (void)fprintf(logger->file, "[INFO] [%s] ", stage);
        (void)vfprintf(logger->file, format, file_arguments);
        fputc('\n', logger->file);
        (void)fflush(logger->file);
    }
    va_end(file_arguments);
    if (active_verbosity >= required) {
        (void)fprintf(stdout, "[INFO] [%s] ", stage);
        (void)vfprintf(stdout, format, arguments);
        fputc('\n', stdout);
        (void)fflush(stdout);
    }
}

void forge_logger_detail(ForgeLogger *logger, const char *stage,
                         const char *format, ...)
{
    va_list arguments;

    va_start(arguments, format);
    write_classified(logger, stage, FORGE_CONSOLE_PROGRESS, format, arguments);
    va_end(arguments);
}

void forge_logger_command(ForgeLogger *logger, const char *stage,
                          const char *format, ...)
{
    va_list arguments;

    va_start(arguments, format);
    write_classified(logger, stage, FORGE_CONSOLE_COMMAND, format, arguments);
    va_end(arguments);
}

void forge_logger_error(ForgeLogger *logger, const char *stage,
                        const char *format, ...)
{
    va_list arguments;
    va_start(arguments, format);
    write_log(logger, stderr, "ERROR", stage, format, arguments);
    va_end(arguments);
}

void forge_logger_close(ForgeLogger *logger)
{
    if (logger != NULL && logger->file != NULL) {
        (void)fclose(logger->file);
        logger->file = NULL;
    }
}
