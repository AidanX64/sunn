#ifdef _MSC_VER
#define _CRT_SECURE_NO_WARNINGS
#endif

#include "forge/platform.h"

#if !FORGE_PLATFORM_WINDOWS && !defined(__APPLE__)
/*
 * st_mtim (nanosecond modification times) needs POSIX.1-2008 on glibc/musl;
 * without this the fallback would silently drop to one-second granularity
 * and quick edit-rebuild cycles could be missed.
 *
 * Apple is excluded on purpose: requesting strict POSIX mode there hides the
 * BSD st_mtimespec member of struct stat (the only nanosecond source macOS
 * provides), so Darwin must keep its default all-visibility environment.
 */
#define _POSIX_C_SOURCE 200809L
#endif

#include <stdio.h>
#include <string.h>

#include "forge/fingerprint.h"
#include "forge/compiler.h"
#include "forge/platform.h"

#if FORGE_PLATFORM_WINDOWS
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#else
#include <sys/stat.h>
#endif

/* Modification time as a comparable pair, at full platform precision. */
typedef struct ForgeFileTime {
    long long seconds;
    long nanoseconds;
} ForgeFileTime;

/* -1 when `path` does not exist or cannot be stat'ed. */
static int file_time(const char *path, ForgeFileTime *time)
{
#if FORGE_PLATFORM_WINDOWS
    WIN32_FILE_ATTRIBUTE_DATA data;
    ULARGE_INTEGER ticks;

    if (GetFileAttributesExA(path, GetFileExInfoStandard, &data) == 0) {
        return -1;
    }
    ticks.HighPart = data.ftLastWriteTime.dwHighDateTime;
    ticks.LowPart = data.ftLastWriteTime.dwLowDateTime;
    /* FILETIME: 100 ns ticks since 1601; convert to seconds + nanoseconds. */
    time->seconds = (long long)(ticks.QuadPart / 10000000ULL);
    time->nanoseconds = (long)((ticks.QuadPart % 10000000ULL) * 100ULL);
    return 0;
#else
    struct stat details;

    if (stat(path, &details) != 0) {
        return -1;
    }
    time->seconds = (long long)details.st_mtime;
#if defined(__APPLE__)
    time->nanoseconds = (long)details.st_mtimespec.tv_nsec;
#elif defined(__linux__) || defined(__FreeBSD__) || defined(__NetBSD__) || \
    defined(__DragonFly__) || defined(__OpenBSD__)
    time->nanoseconds = (long)details.st_mtim.tv_nsec;
#else
    time->nanoseconds = 0L;
#endif
    return 0;
#endif
}

static int time_is_newer(const ForgeFileTime *candidate, const ForgeFileTime *reference)
{
    if (candidate->seconds != reference->seconds) {
        return candidate->seconds > reference->seconds;
    }
    return candidate->nanoseconds > reference->nanoseconds;
}

int forge_fingerprint_path_is_newer(const char *path, const char *reference)
{
    ForgeFileTime path_time;
    ForgeFileTime reference_time;

    if (file_time(path, &path_time) != 0 || file_time(reference, &reference_time) != 0) {
        return 0;
    }
    return time_is_newer(&path_time, &reference_time);
}

int forge_fingerprint_mtime_text(const char *path, char *text, size_t text_size)
{
    ForgeFileTime time;

    if (text == NULL || text_size == 0U) {
        return -1;
    }
    if (file_time(path, &time) != 0) {
        (void)snprintf(text, text_size, "missing");
        return 0;
    }
    (void)snprintf(text, text_size, "%lld.%09ld", time.seconds, time.nanoseconds);
    return 0;
}

/*
 * Stable FNV-1a hash of a text blob. Object files are named after this hash
 * instead of their compile index, so adding/removing/reordering sources never
 * makes an object file "belong" to a different source than the one stored in
 * it (which an index-based scheme would allow once we skip fresh files).
 */
unsigned int forge_fingerprint_hash_text(const char *text)
{
    unsigned int hash = 2166136261U;

    while (*text != '\0') {
        hash ^= (unsigned char)*text++;
        hash *= 16777619U;
    }
    return hash;
}

static unsigned int source_hash(const char *text)
{
    return forge_fingerprint_hash_text(text);
}

/*
 * Compile-command sidecars: "<object>.cmdhash" holds the hash of the exact
 * command line that produced the object. Freshness requires it to match, so
 * editing [profile] cflags or bumping the project version recompiles only
 * what those changes affect, instead of silently relinking stale objects
 * (the failure mode before sidecars existed).
 */
static int command_stamp_path(const char *object_path, char *path, size_t path_size)
{
    size_t length = strlen(object_path);

    if (length + sizeof(".cmdhash") > path_size) {
        return -1;
    }
    memcpy(path, object_path, length + 1U);
    memcpy(path + length, ".cmdhash", sizeof(".cmdhash"));
    return 0;
}

static int command_stamp_matches(const char *object_path, unsigned int command_hash)
{
    char path[FORGE_PATH_MAX];
    FILE *stream;
    unsigned int recorded = 0U;
    int matched;

    if (command_stamp_path(object_path, path, sizeof(path)) != 0) {
        return 0;
    }
    stream = fopen(path, "rb");
    if (stream == NULL) {
        /* Objects built by an older forge carry no stamp; they recompile
         * exactly once — the same contract as objects without depfiles. */
        return 0;
    }
    matched = fscanf(stream, "%x", &recorded) == 1 && recorded == command_hash;
    (void)fclose(stream);
    return matched;
}

void forge_fingerprint_record_command(const char *object_path,
                                      unsigned int command_hash)
{
    char path[FORGE_PATH_MAX];
    FILE *stream;

    if (command_stamp_path(object_path, path, sizeof(path)) != 0) {
        return;
    }
    stream = fopen(path, "wb");
    if (stream == NULL) {
        return; /* best effort: the next run simply recompiles once */
    }
    (void)fprintf(stream, "%08x\n", command_hash);
    (void)fclose(stream);
}

/*
 * Returns 1 when `path` is missing or its mtime is strictly newer than the
 * object file, so the object is stale with respect to that dependency. A
 * missing dependency is treated as stale (conservative): the recompile that
 * follows either fails loudly if the file is still needed, or regenerates a
 * correct dependency list if it is not.
 */
static int dependency_is_stale(const char *path, const char *object_path)
{
    ForgeFileTime path_time;
    ForgeFileTime reference_time;

    /*
     * The contract above is honored here rather than delegated to
     * forge_fingerprint_path_is_newer, which reports "not newer" (0) when
     * either stat fails — treating a deleted header as fresh would leave
     * objects permanently built from a header that no longer exists.
     */
    if (file_time(path, &path_time) != 0) {
        return 1;
    }
    if (file_time(object_path, &reference_time) != 0) {
        return 1;
    }
    return time_is_newer(&path_time, &reference_time);
}

/*
 * Walks a gcc/clang dependency file (Makefile syntax: "target: dep dep \"
 * "...") and returns 1 if any listed file is stale relative to the object.
 * The paths are what the compiler itself resolved during compilation, so this
 * tracks exactly the headers each translation unit used, including ones found
 * through user-provided -I flags.
 */
static int depfile_has_stale_dependency(FILE *stream, const char *object_path)
{
    char token[FORGE_PATH_MAX];
    size_t token_length = 0U;
    int character;

    while ((character = fgetc(stream)) != EOF) {
        if (character == ':') {
            /*
             * The target/rule separator is the colon on the target line, which
             * is followed by a space or a continuation backslash then a newline
             * (e.g. `C:\...\obj.o: \` or `obj.o: dep`). A drive-letter colon
             * (`C:\...`) is followed by more path text, so reading one char
             * ahead tells the two apart; this matters because GCC/Clang emit
             * drive letters here on Windows.
             */
            int next = fgetc(stream);
            int separator = 0;

            if (next == ' ' || next == '\t' || next == '\r' ||
                next == '\n' || next == EOF) {
                separator = 1;
            } else if (next == '\\') {
                int after_backslash = fgetc(stream);
                if (after_backslash == '\r') {
                    after_backslash = fgetc(stream);
                }
                if (after_backslash == '\n') {
                    separator = 1;
                } else if (after_backslash != EOF) {
                    (void)ungetc(after_backslash, stream);
                    (void)ungetc(next, stream);
                    if (token_length + 1U < sizeof(token)) {
                        token[token_length++] = (char)character;
                    }
                    continue;
                }
            } else {
                (void)ungetc(next, stream);
                if (token_length + 1U < sizeof(token)) {
                    token[token_length++] = (char)character;
                }
                continue;
            }
            /* Target/rule separator colon: skip it; the dependencies follow. */
            if (separator) {
                continue;
            }
        }
        /*
         * A backslash at a line end is make's continuation marker; treat it as
         * a separator rather than glue so a depfile without a space before the
         * backslash never joins two paths into one bogus token.
         */
        if (character == '\\') {
            int next = fgetc(stream);
            if (next == '\r') {
                next = fgetc(stream);
            }
            if (next == '\n') {
                if (token_length != 0U) {
                    token[token_length] = '\0';
                    if (dependency_is_stale(token, object_path)) {
                        return 1;
                    }
                    token_length = 0U;
                }
                continue;
            }
            if (next == ' ' || next == '\t') {
                /*
                 * make's escape for a blank inside one path ("\ "): glue it
                 * into the token instead of ending it, or depfiles written
                 * for projects under paths containing spaces would split
                 * into tokens that never stat and look stale forever.
                 */
                if (token_length + 1U < sizeof(token)) {
                    token[token_length++] = (char)next;
                }
                continue;
            }
            if (next != EOF && token_length + 1U < sizeof(token)) {
                token[token_length++] = (char)character;
            }
            if (next != EOF && ungetc(next, stream) == EOF) {
                return 0;
            }
            continue;
        }
        if (character == ' ' || character == '\t' ||
            character == '\r' || character == '\n') {
            if (token_length != 0U) {
                token[token_length] = '\0';
                if (dependency_is_stale(token, object_path)) {
                    return 1;
                }
                token_length = 0U;
            }
            continue;
        }
        if (token_length + 1U < sizeof(token)) {
            token[token_length++] = (char)character;
        }
    }
    if (token_length != 0U) {
        token[token_length] = '\0';
        return dependency_is_stale(token, object_path);
    }
    return 0;
}

int forge_fingerprint_object_fresh(const char *object_path, const char *source_path,
                                   int track_headers, unsigned int command_hash)
{
    char depfile_path[FORGE_PATH_MAX];
    FILE *stream;
    ForgeFileTime object_time;
    ForgeFileTime source_time;
    int fresh;

    if (file_time(object_path, &object_time) != 0 ||
        file_time(source_path, &source_time) != 0) {
        return 0;
    }
    /* Equal timestamps count as fresh: an object written in the same tick as
     * its source was built from that source. */
    if (time_is_newer(&source_time, &object_time)) {
        return 0;
    }
    if (!command_stamp_matches(object_path, command_hash)) {
        return 0;
    }
    if (!track_headers) {
        return 1;
    }
    if (forge_compiler_depfile_path(object_path, depfile_path,
                                    sizeof(depfile_path)) != 0) {
        return 0;
    }
    stream = fopen(depfile_path, "rb");
    if (stream == NULL) {
        return 0;
    }
    fresh = !depfile_has_stale_dependency(stream, object_path);
    (void)fclose(stream);
    return fresh;
}

int forge_fingerprint_object_name(const char *object_directory, const char *source_path,
                                  char (*used)[FORGE_PATH_MAX], size_t used_count,
                                  char *object_path, size_t object_path_size)
{
    unsigned int hash = source_hash(source_path);
    unsigned int attempt = 0U;
    size_t index;

    for (;;) {
        char name[FORGE_PATH_MAX];
        int written;

        if (attempt == 0U) {
            written = snprintf(name, sizeof(name), "%08x.o", hash);
        } else {
            written = snprintf(name, sizeof(name), "%08x-%u.o", hash, attempt);
        }
        if (written < 0 || (size_t)written >= sizeof(name) ||
            snprintf(object_path, object_path_size, "%s/%s", object_directory, name) < 0 ||
            strlen(object_directory) + strlen(name) + 1U >= object_path_size) {
            return -1;
        }
        for (index = 0U; index < used_count; ++index) {
            if (strcmp(used[index], object_path) == 0) {
                break;
            }
        }
        if (index == used_count) {
            break;
        }
        ++attempt;
        if (attempt > 256U) {
            return -1;
        }
    }
    return 0;
}