#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "forge/argv.h"
#include "forge/compiler.h"
#include "forge/debug.h"
#include "forge/platform.h"
#include "forge/process.h"
#include "forge_util.h"

#define FORGE_DEBUG_LINE_MAX 4096U

static int select_debugger(const ForgeHostInfo *host, char *program, size_t program_size,
                           char *error, size_t error_size)
{
    const char *override = getenv("FORGE_DEBUGGER");

    if (override != NULL && override[0] != '\0') {
        if (!forge_util_program_available(override)) {
            forge_util_set_error(error, error_size,
                      "FORGE_DEBUGGER='%s' is not available on PATH", override);
            return -1;
        }
        (void)snprintf(program, program_size, "%s", override);
        return 0;
    }
    if (host->os == FORGE_HOST_OS_WINDOWS && forge_util_program_available("cdb")) {
        (void)snprintf(program, program_size, "cdb");
        return 0;
    }
    if ((host->os == FORGE_HOST_OS_WINDOWS || host->os == FORGE_HOST_OS_LINUX ||
         host->os == FORGE_HOST_OS_UNKNOWN) && forge_util_program_available("gdb")) {
        (void)snprintf(program, program_size, "gdb");
        return 0;
    }
    if ((host->os == FORGE_HOST_OS_MACOS || host->os == FORGE_HOST_OS_UNKNOWN) &&
        forge_util_program_available("lldb")) {
        (void)snprintf(program, program_size, "lldb");
        return 0;
    }
    forge_util_set_error(error, error_size,
              "no supported debugger was found; install cdb, gdb, or lldb, "
              "or set FORGE_DEBUGGER to its executable name");
    return -1;
}

/* Which command dialect the chosen debugger speaks. */
typedef enum ForgeDebuggerFlavor {
    FORGE_DEBUGGER_FLAVOR_GDB,
    FORGE_DEBUGGER_FLAVOR_LLDB,
    FORGE_DEBUGGER_FLAVOR_CDB
} ForgeDebuggerFlavor;

/*
 * Classifies a debugger executable by its file name, not by exact string
 * equality: FORGE_DEBUGGER=/usr/bin/lldb-18 or gdb-multiarch must pick the
 * matching flag set. Versioned and prefixed names are common enough that
 * prefix matching on the basename is the pragmatic rule.
 */
static ForgeDebuggerFlavor debugger_flavor(const char *program)
{
    const char *base = program;
    const char *cursor;
    size_t length;

    for (cursor = program; *cursor != '\0'; ++cursor) {
        if (*cursor == '/' || *cursor == '\\') {
            base = cursor + 1;
        }
    }
    length = strlen(base);
    if (length > 4U && forge_util_has_suffix(base, ".exe")) {
        length -= 4U;
    }
    if (length >= 3U && strncmp(base, "cdb", 3U) == 0) {
        return FORGE_DEBUGGER_FLAVOR_CDB;
    }
    if (length >= 4U && strncmp(base, "lldb", 4U) == 0) {
        return FORGE_DEBUGGER_FLAVOR_LLDB;
    }
    return FORGE_DEBUGGER_FLAVOR_GDB;
}

/* Builds the argv that runs the chosen debugger in batch mode. */
static int make_debug_argv(const char *program, const char *executable_path,
                           ForgeArgv *argv, char *error, size_t error_size)
{
    switch (debugger_flavor(program)) {
    case FORGE_DEBUGGER_FLAVOR_CDB:
        if (forge_argv_append(argv, program) != 0 ||
            forge_argv_append(argv, "-lines") != 0 ||
            forge_argv_append(argv, "-c") != 0 ||
            forge_argv_append(argv, "g; k; u @rip L20; q") != 0 ||
            forge_argv_append(argv, executable_path) != 0) {
            forge_util_set_error(error, error_size, "out of memory while building debugger command");
            return -1;
        }
        break;
    case FORGE_DEBUGGER_FLAVOR_LLDB:
        if (forge_argv_append(argv, program) != 0 ||
            forge_argv_append(argv, "--batch") != 0 ||
            forge_argv_append(argv, "-o") != 0 ||
            forge_argv_append(argv, "run") != 0 ||
            forge_argv_append(argv, "-o") != 0 ||
            forge_argv_append(argv, "bt") != 0 ||
            forge_argv_append(argv, "-o") != 0 ||
            forge_argv_append(argv, "disassemble -n main") != 0 ||
            forge_argv_append(argv, executable_path) != 0) {
            forge_util_set_error(error, error_size, "out of memory while building debugger command");
            return -1;
        }
        break;
    default:
        /* gdb: `disassemble /s` is the modern spelling; /m is deprecated. */
        if (forge_argv_append(argv, program) != 0 ||
            forge_argv_append(argv, "--batch") != 0 ||
            forge_argv_append(argv, "-q") != 0 ||
            forge_argv_append(argv, "-ex") != 0 ||
            forge_argv_append(argv, "run") != 0 ||
            forge_argv_append(argv, "-ex") != 0 ||
            forge_argv_append(argv, "bt") != 0 ||
            forge_argv_append(argv, "-ex") != 0 ||
            forge_argv_append(argv, "disassemble /s main") != 0 ||
            forge_argv_append(argv, executable_path) != 0) {
            forge_util_set_error(error, error_size, "out of memory while building debugger command");
            return -1;
        }
        break;
    }
    return 0;
}

static int is_stack_line(const char *line)
{
    return line[0] == '#' || strstr(line, "frame #") != NULL ||
           strstr(line, "Call Site") != NULL || strstr(line, " at ") != NULL;
}

static int is_disassembly_line(const char *line)
{
    return strstr(line, "Dump of assembler") != NULL ||
           strstr(line, "disassembly") != NULL || strstr(line, "Disassembly") != NULL ||
           strncmp(line, "=>", 2U) == 0 ||
           (isxdigit((unsigned char)line[0]) && strstr(line, "<+") != NULL);
}

static void postprocess_debug_output(const char *raw_path, ForgeLogger *logger)
{
    FILE *raw = fopen(raw_path, "r");
    char line[FORGE_DEBUG_LINE_MAX];
    size_t stack_lines = 0U;
    size_t disassembly_lines = 0U;

    if (raw == NULL) {
        forge_logger_error(logger, "debug", "could not read debugger transcript: %s", raw_path);
        return;
    }
    forge_logger_log(logger, "debug", "----- lifter-style stack and disassembly view -----");
    while (fgets(line, sizeof(line), raw) != NULL) {
        char *text = forge_util_trim(line);

        if (is_stack_line(text) && stack_lines < 64U) {
            forge_logger_log(logger, "stack", "%s", text);
            ++stack_lines;
        } else if (is_disassembly_line(text) && disassembly_lines < 64U) {
            forge_logger_log(logger, "disassembly", "%s", text);
            ++disassembly_lines;
        }
    }
    if (stack_lines == 0U && disassembly_lines == 0U) {
        forge_logger_log(logger, "debug",
                         "no stack or disassembly lines were recognized; "
                         "raw debugger transcript retained at %s", raw_path);
    }
    (void)fclose(raw);
}

int forge_debug_launch(const char *executable_path, ForgeLogger *logger,
                       char *error, size_t error_size)
{
    ForgeHostInfo host;
    char debugger[FORGE_COMPILER_VALUE_MAX];
    ForgeArgv argv = {0};
    char raw_path[FORGE_LOG_PATH_MAX + 8U];
    char display[FORGE_LOG_PATH_MAX + 512U];
    int exit_code = 0;
    int written;

    if (executable_path == NULL || executable_path[0] == '\0' || logger == NULL) {
        forge_util_set_error(error, error_size, "executable path and active logger are required");
        return -1;
    }
    if (forge_detect_host(&host, error, error_size) != 0 ||
        select_debugger(&host, debugger, sizeof(debugger), error, error_size) != 0 ||
        make_debug_argv(debugger, executable_path, &argv, error, error_size) != 0) {
        forge_argv_free(&argv);
        return -1;
    }
    written = snprintf(raw_path, sizeof(raw_path), "%s.raw", logger->path);
    if (written < 0 || (size_t)written >= sizeof(raw_path)) {
        forge_util_set_error(error, error_size, "raw debugger transcript path is too long");
        forge_argv_free(&argv);
        return -1;
    }
    forge_logger_log(logger, "debug", "debugger=%s executable=%s", debugger, executable_path);
    if (forge_argv_join(display, sizeof(display), &argv) == 0) {
        forge_logger_log(logger, "debug", "command: %s", display);
    }
    (void)forge_argv_finalize(&argv);
    if (forge_process_run(argv.items, raw_path, 0, &exit_code, error, error_size) != 0) {
        forge_argv_free(&argv);
        return -1;
    }
    forge_argv_free(&argv);
    if (exit_code != 0) {
        forge_logger_error(logger, "debug",
                           "debugger exited with an error; processing its transcript");
    }
    postprocess_debug_output(raw_path, logger);
    return 0;
}