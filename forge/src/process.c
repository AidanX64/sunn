#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "forge/platform.h"
#include "forge/process.h"

#if FORGE_PLATFORM_WINDOWS
#define WIN32_LEAN_AND_MEAN
#include <windows.h>

/*
 * Quotes one argument for a CreateProcess command line. The rule mirrors the
 * C runtime's parsing: an empty argument becomes ""; an argument containing a
 * space or tab is wrapped in quotes, and each embedded quote or run of
 * trailing backslashes is escaped by doubling backslashes before a quote.
 * Returns the number of characters written, or -1 if it would not fit.
 */
static int append_quoted(char *destination, size_t destination_size, const char *text)
{
    static const char *const KNOWN_SAFE = " \t";
    size_t length = 0U;
    int needs_quotes;
    const char *cursor;
    size_t run = 0U;

    if (text == NULL) {
        text = "";
    }
    if (text[0] == '\0') {
        needs_quotes = 1;
    } else {
        needs_quotes = strpbrk(text, KNOWN_SAFE) != NULL;
        for (cursor = text; *cursor != '\0' && !needs_quotes; ++cursor) {
            if (*cursor == '"') {
                needs_quotes = 1;
            }
        }
    }
    if (!needs_quotes && destination_size > 0U) {
        /* No quoting needed; the argument is literal. */
        size_t text_length = strlen(text);
        if (text_length + 1U > destination_size) {
            return -1;
        }
        (void)memcpy(destination, text, text_length);
        destination[text_length] = '\0';
        return (int)text_length;
    }

    if (2U + strlen(text) > destination_size) {
        return -1;
    }
    destination[length++] = '"';
    for (cursor = text; *cursor != '\0'; ++cursor) {
        if (*cursor == '"') {
            /* The backslashes immediately before the quote get doubled. */
            if (run != 0U) {
                while (run != 0U) {
                    if (length + 2U > destination_size) {
                        return -1;
                    }
                    destination[length++] = '\\';
                    destination[length++] = '\\';
                    --run;
                }
                run = 0U;
            }
            if (length + 2U > destination_size) {
                return -1;
            }
            destination[length++] = '\\';
            destination[length++] = '"';
        } else if (*cursor == '\\') {
            ++run;
        } else {
            if (run != 0U) {
                while (run != 0U) {
                    if (length + 1U > destination_size) {
                        return -1;
                    }
                    destination[length++] = '\\';
                    --run;
                }
                run = 0U;
            }
            if (length + 1U > destination_size) {
                return -1;
            }
            destination[length++] = *cursor;
        }
    }
    if (run != 0U) {
        /* Trailing backslashes are doubled so they survive quote parsing. */
        while (run != 0U) {
            if (length + 2U > destination_size) {
                return -1;
            }
            destination[length++] = '\\';
            destination[length++] = '\\';
            --run;
        }
        run = 0U;
    }
    if (length + 1U > destination_size) {
        return -1;
    }
    destination[length++] = '"';
    destination[length] = '\0';
    return (int)length;
}

static char *build_command_line(char *const *argv, char *error, size_t error_size)
{
    size_t index;
    size_t length = 0U;
    size_t capacity = 128U + strlen(argv[0]) * 2U;
    char *line;
    int written;

    /* Quoting can more than double an argument: every backslash before a
     * quote is doubled and quotes gain a backslash each, so budget 2x plus
     * slack instead of the naive strlen + 1 that made long path-heavy
     * commands fail spuriously with "command line is too long". */
    for (index = 1; index < (size_t)-1; ++index) {
        if (argv[index] == NULL) {
            break;
        }
        capacity += strlen(argv[index]) * 2U + 4U;
    }
    line = malloc(capacity);
    if (line == NULL) {
        if (error != NULL && error_size != 0U) {
            (void)snprintf(error, error_size, "out of memory while assembling command line");
        }
        return NULL;
    }
    line[0] = '\0';
    for (index = 0; index < (size_t)-1; ++index) {
        if (argv[index] == NULL) {
            break;
        }
        if (index != 0U) {
            /* The capacity above is budgeted for every argument, so hitting
             * this means the math was wrong: fail loudly rather than hand
             * the child a silently truncated command line. */
            if (length + 1U >= capacity) {
                if (error != NULL && error_size != 0U) {
                    (void)snprintf(error, error_size, "command line is too long");
                }
                free(line);
                return NULL;
            }
            line[length++] = ' ';
            line[length] = '\0';
        }
        written = append_quoted(line + length, capacity - length, argv[index]);
        if (written < 0) {
            if (error != NULL && error_size != 0U) {
                (void)snprintf(error, error_size, "command line is too long");
            }
            free(line);
            return NULL;
        }
        length += (size_t)written;
    }
    return line;
}

#define FORGE_PROCESS_PATH_MAX 1024U

/*
 * S4 gate: with a NULL application name, CreateProcess resolves a bare
 * program name against the calling application's directory and the current
 * working directory before PATH — so a hostile repository containing
 * git.exe/make.exe/cmake.exe could shadow the real toolchain for any forge
 * invocation made from inside it. Resolve the program the way execvp does
 * instead: an explicit path is canonicalized as given, otherwise each PATH
 * entry is probed and the current directory is never consulted implicitly.
 * The absolute result is pinned via lpApplicationName so CreateProcess takes
 * it verbatim instead of re-running its own search.
 */
static int file_exists_program(const char *path)
{
    DWORD attributes = GetFileAttributesA(path);

    return attributes != INVALID_FILE_ATTRIBUTES &&
           (attributes & FILE_ATTRIBUTE_DIRECTORY) == 0U;
}

static int probe_program(const char *directory, size_t directory_length,
                         const char *program, char *resolved,
                         size_t resolved_size)
{
    int needs_separator = directory_length != 0U &&
                          directory[directory_length - 1U] != '/' &&
                          directory[directory_length - 1U] != '\\';

    if (snprintf(resolved, resolved_size, "%.*s%s%s",
                 (int)directory_length, directory,
                 needs_separator ? "\\" : "", program) < 0 ||
        strlen(resolved) >= resolved_size) {
        return 0;
    }
    if (file_exists_program(resolved)) {
        return 1;
    }
    if (snprintf(resolved, resolved_size, "%.*s%s%s.exe",
                 (int)directory_length, directory,
                 needs_separator ? "\\" : "", program) < 0 ||
        strlen(resolved) >= resolved_size) {
        return 0;
    }
    return file_exists_program(resolved);
}

static int resolve_executable(const char *program, char *resolved,
                              size_t resolved_size, char *error,
                              size_t error_size)
{
    const char *path_value;
    const char *cursor;

    if (strchr(program, '/') != NULL || strchr(program, '\\') != NULL ||
        (program[0] != '\0' && program[1] == ':')) {
        DWORD length = GetFullPathNameA(program, (DWORD)resolved_size,
                                        resolved, NULL);

        if (length == 0U || length >= resolved_size) {
            (void)snprintf(error, error_size, "program path is too long: '%s'",
                           program);
            return -1;
        }
        if (!file_exists_program(resolved)) {
            (void)snprintf(error, error_size, "program not found: '%s'",
                           resolved);
            return -1;
        }
        return 0;
    }
    path_value = getenv("PATH");
    if (path_value == NULL) {
        (void)snprintf(error, error_size, "'%s' was not found (PATH is not set)",
                       program);
        return -1;
    }
    cursor = path_value;
    for (;;) {
        const char *end = strchr(cursor, ';');
        size_t entry_length;
        const char *entry = cursor;
        size_t trimmed_length;

        if (end == NULL) {
            end = cursor + strlen(cursor);
        }
        entry_length = (size_t)(end - cursor);
        /* Tolerate quoted entries ("C:\some dir") by skipping the quotes. */
        while (entry_length >= 2U && entry[0] == '"' &&
               entry[entry_length - 1U] == '"') {
            ++entry;
            entry_length -= 2U;
        }
        trimmed_length = entry_length;
        while (trimmed_length > 0U &&
               (entry[trimmed_length - 1U] == ' ' ||
                entry[trimmed_length - 1U] == '\t')) {
            --trimmed_length;
        }
        if (trimmed_length != 0U &&
            probe_program(entry, trimmed_length, program, resolved,
                          resolved_size)) {
            return 0;
        }
        if (*end == '\0') {
            break;
        }
        cursor = end + 1;
    }
    (void)snprintf(error, error_size,
                   "'%s' was not found in PATH; install it or extend PATH",
                   program);
    return -1;
}

int forge_process_run(char *const *argv, const char *redirect_to,
                      int appending, int *exit_code, char *error, size_t error_size)
{
    char *command_line;
    char executable[FORGE_PROCESS_PATH_MAX];
    HANDLE redirected = NULL;
    STARTUPINFOA startup;
    PROCESS_INFORMATION process;
    DWORD code;

    if (argv == NULL || argv[0] == NULL || exit_code == NULL) {
        if (error != NULL && error_size != 0U) {
            (void)snprintf(error, error_size, "argv and exit code output are required");
        }
        return -1;
    }
    if (resolve_executable(argv[0], executable, sizeof(executable),
                           error, error_size) != 0) {
        return -1;
    }
    if (redirect_to != NULL) {
        redirected = CreateFileA(redirect_to,
                                 FILE_APPEND_DATA,
                                 FILE_SHARE_READ | FILE_SHARE_WRITE,
                                 NULL, appending ? OPEN_ALWAYS : CREATE_ALWAYS,
                                 FILE_ATTRIBUTE_NORMAL, NULL);
        if (redirected == INVALID_HANDLE_VALUE) {
            if (error != NULL && error_size != 0U) {
                (void)snprintf(error, error_size, "could not open '%s' for output (error %lu)",
                               redirect_to, (unsigned long)GetLastError());
            }
            return -1;
        }
        (void)SetHandleInformation(redirected, HANDLE_FLAG_INHERIT, HANDLE_FLAG_INHERIT);
    }
    command_line = build_command_line(argv, error, error_size);
    if (command_line == NULL) {
        if (redirected != NULL) {
            (void)CloseHandle(redirected);
        }
        return -1;
    }
    (void)memset(&startup, 0, sizeof(startup));
    startup.cb = sizeof(startup);
    startup.dwFlags = STARTF_USESTDHANDLES;
    startup.hStdInput = GetStdHandle(STD_INPUT_HANDLE);
    startup.hStdOutput = redirected != NULL ? redirected : GetStdHandle(STD_OUTPUT_HANDLE);
    startup.hStdError = redirected != NULL ? redirected : GetStdHandle(STD_ERROR_HANDLE);

    if (CreateProcessA(executable, command_line, NULL, NULL, TRUE, 0, NULL,
                       NULL, &startup, &process) == 0) {
        if (error != NULL && error_size != 0U) {
            (void)snprintf(error, error_size,
                           "could not start '%s' (error %lu)", argv[0],
                           (unsigned long)GetLastError());
        }
        free(command_line);
        if (redirected != NULL) {
            (void)CloseHandle(redirected);
        }
        return -1;
    }
    (void)WaitForSingleObject(process.hProcess, INFINITE);
    if (GetExitCodeProcess(process.hProcess, &code) == 0) {
        code = (DWORD)-1;
    }
    *exit_code = (int)code;
    (void)CloseHandle(process.hThread);
    (void)CloseHandle(process.hProcess);
    free(command_line);
    if (redirected != NULL) {
        (void)CloseHandle(redirected);
    }
    return 0;
}
#else
#include <errno.h>
#include <fcntl.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>

int forge_process_run(char *const *argv, const char *redirect_to,
                      int appending, int *exit_code, char *error, size_t error_size)
{
    pid_t child;
    int status = -1;
    int fd = -1;

    if (argv == NULL || argv[0] == NULL || exit_code == NULL) {
        if (error != NULL && error_size != 0U) {
            (void)snprintf(error, error_size, "argv and exit code output are required");
        }
        return -1;
    }
    if (redirect_to != NULL) {
        fd = open(redirect_to, O_WRONLY | O_CREAT | (appending ? O_APPEND : O_TRUNC), 0644);
        if (fd < 0) {
            if (error != NULL && error_size != 0U) {
                (void)snprintf(error, error_size, "could not open '%s' for output: %s",
                               redirect_to, strerror(errno));
            }
            return -1;
        }
    }
    child = fork();
    if (child < 0) {
        if (fd >= 0) {
            (void)close(fd);
        }
        if (error != NULL && error_size != 0U) {
            (void)snprintf(error, error_size, "could not fork: %s", strerror(errno));
        }
        return -1;
    }
    if (child == 0) {
        if (fd >= 0) {
            (void)dup2(fd, STDOUT_FILENO);
            (void)dup2(fd, STDERR_FILENO);
            (void)close(fd);
        }
        execvp(argv[0], (char *const *)argv);
        _exit(127);
    }
    if (fd >= 0) {
        (void)close(fd);
    }
    while (waitpid(child, &status, 0) < 0 && errno == EINTR) {
    }
    if (WIFEXITED(status)) {
        *exit_code = WEXITSTATUS(status);
    } else {
        *exit_code = -1;
    }
    return 0;
}
#endif