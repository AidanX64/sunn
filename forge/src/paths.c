#ifdef _MSC_VER
#define _CRT_SECURE_NO_WARNINGS
#endif

#include "forge/platform.h"

#if !FORGE_PLATFORM_WINDOWS
#define _XOPEN_SOURCE 700
#define _POSIX_C_SOURCE 200809L
#endif

#include <ctype.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "forge/paths.h"
#include "forge_util.h"

#if FORGE_PLATFORM_WINDOWS
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#else
#include <dirent.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>
#endif

int forge_paths_join(char *destination, size_t destination_size,
                     const char *left, const char *right)
{
    int written = snprintf(destination, destination_size, "%s/%s", left, right);

    return written < 0 || (size_t)written >= destination_size ? -1 : 0;
}

static int is_absolute_path(const char *path)
{
#if FORGE_PLATFORM_WINDOWS
    return (isalpha((unsigned char)path[0]) && path[1] == ':') ||
           path[0] == '/' || path[0] == '\\';
#else
    return path[0] == '/';
#endif
}

int forge_paths_resolve(const char *root, const char *relative,
                        char *destination, size_t destination_size)
{
    if (is_absolute_path(relative)) {
        int written = snprintf(destination, destination_size, "%s", relative);
        return written < 0 || (size_t)written >= destination_size ? -1 : 0;
    }
    return forge_paths_join(destination, destination_size, root, relative);
}

int forge_paths_absolute(const char *path, char *destination,
                         size_t destination_size)
{
    char full[FORGE_PATH_MAX];

    if (path == NULL || destination == NULL) {
        return -1;
    }
#if FORGE_PLATFORM_WINDOWS
    {
        DWORD length = GetFullPathNameA(path, sizeof(full), full, NULL);
        if (length == 0U || length >= sizeof(full)) {
            full[0] = '\0';
        }
    }
#else
    if (realpath(path, full) == NULL) {
        full[0] = '\0';
    }
#endif
    /* Unresolvable paths keep their raw text so callers still get a usable
     * key; the stamp comparison just stays sensitive to the exact spelling. */
    if (full[0] == '\0' && snprintf(full, sizeof(full), "%s", path) < 0) {
        return -1;
    }
    if (strlen(full) >= destination_size) {
        return -1;
    }
    (void)snprintf(destination, destination_size, "%s", full);
    return 0;
}

int forge_paths_project_root(const char *manifest_path, char *root,
                             size_t root_size, char *error, size_t error_size)
{
    char full[FORGE_PATH_MAX];
    char *separator;

    if (manifest_path == NULL || root == NULL) {
        forge_util_set_error(error, error_size, "manifest path and output are required");
        return -1;
    }
#if FORGE_PLATFORM_WINDOWS
    {
        DWORD length = GetFullPathNameA(manifest_path, sizeof(full), full, NULL);
        if (length == 0U || length >= sizeof(full)) {
            forge_util_set_error(error, error_size,
                                 "could not resolve manifest path '%s'", manifest_path);
            return -1;
        }
    }
#else
    if (realpath(manifest_path, full) == NULL) {
        if (snprintf(full, sizeof(full), "%s", manifest_path) < 0 ||
            strlen(manifest_path) >= sizeof(full)) {
            forge_util_set_error(error, error_size,
                                 "could not resolve manifest path '%s'", manifest_path);
            return -1;
        }
    }
#endif
    if (strlen(full) >= root_size) {
        forge_util_set_error(error, error_size, "manifest path is too long");
        return -1;
    }
    separator = strrchr(full, '/');
#if FORGE_PLATFORM_WINDOWS
    {
        char *backslash = strrchr(full, '\\');
        if (backslash != NULL && backslash > separator) {
            separator = backslash;
        }
    }
#endif
    if (separator == NULL) {
        (void)snprintf(root, root_size, ".");
        return 0;
    }
    *separator = '\0';
    (void)snprintf(root, root_size, "%s", full);
    return 0;
}

/* Truncates `path` in place to its parent directory. Returns 0 when a parent
 * was produced and -1 when `path` is already top-most ("/", "C:\", or a bare
 * relative component). */
static int parent_directory(char *path)
{
    size_t length = strlen(path);
    size_t index = length;

    while (index > 0U && path[index - 1U] != '/' && path[index - 1U] != '\\') {
        --index;
    }
    if (index == 0U) {
        return -1;
    }
    /* Keep the separator itself only at a filesystem root ("C:\", "/"). */
    if (index == 1U || (index == 3U && path[1] == ':')) {
        if (strlen(path) == index) {
            return -1;
        }
        path[index] = '\0';
    } else {
        path[index - 1U] = '\0';
    }
    return 0;
}

int forge_paths_find_manifest(const char *manifest_name, char *found,
                              size_t found_size)
{
    char current[FORGE_PATH_MAX];
    char candidate[FORGE_PATH_MAX];

    if (forge_paths_current_directory(current, sizeof(current)) != 0) {
        return -1;
    }
    for (;;) {
        FILE *probe;
        size_t length_before = strlen(current);

        if (forge_paths_join(candidate, sizeof(candidate), current,
                             manifest_name) != 0) {
            return -1;
        }
        probe = fopen(candidate, "r");
        if (probe != NULL) {
            (void)fclose(probe);
            if (snprintf(found, found_size, "%s", candidate) < 0 ||
                strlen(candidate) >= found_size) {
                return -1;
            }
            return 1;
        }
        if (parent_directory(current) != 0 || strlen(current) >= length_before) {
            return 0;
        }
    }
}

int forge_paths_current_directory(char *buffer, size_t buffer_size)
{
#if FORGE_PLATFORM_WINDOWS
    /* Returns the length written, or the required size when the buffer is
     * too small; either way a value >= buffer_size means it did not fit. */
    DWORD length = GetCurrentDirectoryA((DWORD)buffer_size, buffer);

    if (length == 0U || length >= (DWORD)buffer_size) {
        return -1;
    }
#else
    if (getcwd(buffer, buffer_size) == NULL) {
        return -1;
    }
#endif
    return 0;
}

static int make_directory(const char *path, char *error, size_t error_size)
{
#if FORGE_PLATFORM_WINDOWS
    if (CreateDirectoryA(path, NULL) == 0 && GetLastError() != ERROR_ALREADY_EXISTS) {
        forge_util_set_error(error, error_size,
                  "could not create output directory '%s' (error %lu)", path,
                  (unsigned long)GetLastError());
        return -1;
    }
#else
    if (mkdir(path, 0777) != 0 && errno != EEXIST) {
        forge_util_set_error(error, error_size,
                  "could not create output directory '%s': %s", path, strerror(errno));
        return -1;
    }
#endif
    return 0;
}

int forge_paths_ensure_directory(const char *path, char *error, size_t error_size)
{
    char current[FORGE_PATH_MAX];
    size_t index;
    size_t length;

    if (path == NULL) {
        forge_util_set_error(error, error_size, "invalid output directory path");
        return -1;
    }
    length = strlen(path);
    if (length == 0U || length >= sizeof(current)) {
        forge_util_set_error(error, error_size, "invalid output directory path");
        return -1;
    }
    (void)snprintf(current, sizeof(current), "%s", path);
    for (index = 1U; index <= length; ++index) {
        if (current[index] != '/' && current[index] != '\\' && current[index] != '\0') {
            continue;
        }
        current[index] = '\0';
        if (make_directory(current, error, error_size) != 0) {
            return -1;
        }
        current[index] = path[index];
    }
    return 0;
}

#if FORGE_PLATFORM_WINDOWS
static int remove_tree_recursive(const char *path, char *error, size_t error_size)
{
    WIN32_FIND_DATAA entry;
    HANDLE handle;
    char pattern[FORGE_PATH_MAX];

    if (forge_paths_join(pattern, sizeof(pattern), path, "*") != 0) {
        forge_util_set_error(error, error_size, "clean path is too long: %s", path);
        return -1;
    }
    handle = FindFirstFileA(pattern, &entry);
    if (handle == INVALID_HANDLE_VALUE) {
        if (GetLastError() == ERROR_FILE_NOT_FOUND ||
            GetLastError() == ERROR_PATH_NOT_FOUND) {
            return 0;
        }
        forge_util_set_error(error, error_size,
                  "could not open directory to clean '%s' (error %lu)", path,
                  (unsigned long)GetLastError());
        return -1;
    }
    do {
        char child[FORGE_PATH_MAX];

        if (strcmp(entry.cFileName, ".") == 0 || strcmp(entry.cFileName, "..") == 0) {
            continue;
        }
        if (forge_paths_join(child, sizeof(child), path, entry.cFileName) != 0) {
            forge_util_set_error(error, error_size,
                                 "clean path is too long under '%s'", path);
            (void)FindClose(handle);
            return -1;
        }
        if ((entry.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0U &&
            (entry.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) == 0U) {
            /*
             * A directory junction or symlink carries the reparse attribute:
             * deleting the link itself must never descend into what it
             * points at, or clean would delete files far outside target/.
             */
            if (remove_tree_recursive(child, error, error_size) != 0) {
                (void)FindClose(handle);
                return -1;
            }
        } else if ((entry.dwFileAttributes & (FILE_ATTRIBUTE_DIRECTORY |
                                              FILE_ATTRIBUTE_REPARSE_POINT)) != 0U) {
            /* Directory link: RemoveDirectoryA unlinks it without following. */
            if (RemoveDirectoryA(child) == 0) {
                forge_util_set_error(error, error_size,
                                     "could not remove directory link '%s' (error %lu)",
                                     child, (unsigned long)GetLastError());
                (void)FindClose(handle);
                return -1;
            }
        } else if (DeleteFileA(child) == 0) {
            forge_util_set_error(error, error_size,
                                 "could not delete file '%s' (error %lu)", child,
                                 (unsigned long)GetLastError());
            (void)FindClose(handle);
            return -1;
        }
    } while (FindNextFileA(handle, &entry) != 0);
    if (GetLastError() != ERROR_NO_MORE_FILES) {
        forge_util_set_error(error, error_size,
                             "could not finish cleaning '%s' (error %lu)", path,
                             (unsigned long)GetLastError());
        (void)FindClose(handle);
        return -1;
    }
    (void)FindClose(handle);
    if (RemoveDirectoryA(path) == 0) {
        forge_util_set_error(error, error_size,
                             "could not remove directory '%s' (error %lu)", path,
                             (unsigned long)GetLastError());
        return -1;
    }
    return 0;
}
#else
static int remove_tree_recursive(const char *path, char *error, size_t error_size)
{
    DIR *stream = opendir(path);
    struct dirent *entry;

    if (stream == NULL) {
        if (errno == ENOENT) {
            return 0;
        }
        forge_util_set_error(error, error_size,
                  "could not open directory to clean '%s': %s", path, strerror(errno));
        return -1;
    }
    while ((entry = readdir(stream)) != NULL) {
        char child[FORGE_PATH_MAX];
        struct stat details;

        if (strcmp(entry->d_name, ".") == 0 || strcmp(entry->d_name, "..") == 0) {
            continue;
        }
        if (forge_paths_join(child, sizeof(child), path, entry->d_name) != 0) {
            forge_util_set_error(error, error_size,
                                 "clean path is too long under '%s'", path);
            (void)closedir(stream);
            return -1;
        }
        /*
         * lstat, not stat: a symlink must be unlinked as the link itself.
         * stat would follow it, so a symlink under target/ pointing at a
         * real directory would get its *contents* deleted outside target/.
         */
        if (lstat(child, &details) != 0) {
            forge_util_set_error(error, error_size,
                                 "could not inspect '%s': %s", child, strerror(errno));
            (void)closedir(stream);
            return -1;
        }
        if (S_ISDIR(details.st_mode)) {
            if (remove_tree_recursive(child, error, error_size) != 0) {
                (void)closedir(stream);
                return -1;
            }
        } else if (unlink(child) != 0) {
            forge_util_set_error(error, error_size,
                                 "could not delete '%s': %s", child, strerror(errno));
            (void)closedir(stream);
            return -1;
        }
    }
    (void)closedir(stream);
    if (rmdir(path) != 0) {
        forge_util_set_error(error, error_size,
                             "could not remove directory '%s': %s", path, strerror(errno));
        return -1;
    }
    return 0;
}
#endif

int forge_paths_remove_tree(const char *path, char *error, size_t error_size)
{
    if (path == NULL) {
        forge_util_set_error(error, error_size, "clean path is required");
        return -1;
    }
    return remove_tree_recursive(path, error, error_size);
}

void forge_paths_safe_output_name(const char *project_name, char *output,
                                  size_t output_size)
{
    size_t index;

    if (output == NULL || output_size == 0U) {
        return;
    }
    for (index = 0U; index + 1U < output_size && project_name != NULL &&
                     project_name[index] != '\0';
         ++index) {
        unsigned char character = (unsigned char)project_name[index];
        output[index] = isalnum(character) || character == '-' || character == '_' ||
                        character == '.' ? (char)character : '-';
    }
    output[index] = '\0';
}