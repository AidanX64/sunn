#include "forge/fetch.h"

#include "forge/platform.h"

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "forge/argv.h"
#include "forge/paths.h"
#include "forge/process.h"
#include "forge_util.h"

/* Spawns argv (already finalized) and maps the outcome onto 0/-1 with a
 * readable error. `work_dir` (NULL to inherit) becomes the child's working
 * directory; command lines go through forge_logger_command so -vv shows
 * exactly what ran; failures name the tool and its exit code. */
static int run_tool(ForgeLogger *logger, const char *tool,
                    ForgeArgv *argv, const char *work_dir, char *error,
                    size_t error_size)
{
    char display[FORGE_PATH_MAX * 2U];
    int exit_code = 0;
    int status;

    if (forge_argv_finalize(argv) != 0) {
        forge_util_set_error(error, error_size,
                  "out of memory while building a %s command", tool);
        return -1;
    }
    if (forge_argv_join(display, sizeof(display), argv) == 0) {
        forge_logger_command(logger, "deps", "%s", display);
    }
    status = forge_process_run_at(work_dir, argv->items, NULL, 0,
                                   &exit_code, error, error_size);
    if (status != 0) {
        return -1;
    }
    if (exit_code != 0) {
        forge_util_set_error(error, error_size, "%s failed (exit %d)",
                  tool, exit_code);
        return -1;
    }
    return 0;
}

static int is_scheme_character(unsigned char character, int first)
{
    if (isalpha(character)) {
        return 1;
    }
    return !first && (isdigit(character) || character == '+' ||
                      character == '-' || character == '.');
}

/* Compares a scheme prefix case-insensitively (transports are lowercase by
 * convention, but be liberal here and strict about the host instead). */
static int scheme_matches(const char *url, size_t length, const char *name)
{
    size_t index;

    for (index = 0U; index < length; ++index) {
        if (tolower((unsigned char)url[index]) != name[index]) {
            return 0;
        }
    }
    return name[length] == '\0';
}

/* True for the loopback hosts http:// is allowed to talk to (tests). */
static int host_is_loopback(const char *host, size_t length)
{
    char bare[FORGE_PATH_MAX];
    size_t index;

    if (length == 0U || length >= sizeof(bare)) {
        return 0;
    }
    memcpy(bare, host, length);
    bare[length] = '\0';
    /* Strip one layer of IPv6 brackets: [::1]. */
    if (bare[0] == '[') {
        size_t end = strlen(bare);

        if (end < 2U || bare[end - 1U] != ']') {
            return 0;
        }
        bare[end - 1U] = '\0';
        memmove(bare, bare + 1U, end - 1U);
    }
    for (index = 0U; bare[index] != '\0'; ++index) {
        bare[index] = (char)tolower((unsigned char)bare[index]);
    }
    return strcmp(bare, "localhost") == 0 || strcmp(bare, "127.0.0.1") == 0 ||
           strcmp(bare, "::1") == 0;
}

int forge_fetch_url_is_supported(const char *url, char *error,
                                 size_t error_size)
{
    const char *colon;
    const char *cursor;
    size_t scheme_length;
    int looks_like_scheme;

    if (url == NULL || url[0] == '\0') {
        forge_util_set_error(error, error_size, "registry URL is empty");
        return -1;
    }
    if (url[0] == '-') {
        forge_util_set_error(error, error_size,
                  "registry URL '%s' is option-shaped; refusing to hand it "
                  "to a download tool", url);
        return -1;
    }
    if (strlen(url) >= FORGE_PATH_MAX) {
        forge_util_set_error(error, error_size, "registry URL is too long");
        return -1;
    }
    colon = strchr(url, ':');
    if (colon == NULL) {
        forge_util_set_error(error, error_size,
                  "registry URL '%s' has no transport prefix (https://, "
                  "http://localhost, or file:// with "
                  "FORGE_ALLOW_UNSAFE_REGISTRY=1)", url);
        return -1;
    }
    scheme_length = (size_t)(colon - url);
    looks_like_scheme = is_scheme_character((unsigned char)url[0], 1);
    for (cursor = url + 1; looks_like_scheme && cursor < colon; ++cursor) {
        looks_like_scheme = is_scheme_character((unsigned char)*cursor, 0);
    }
    if (!looks_like_scheme) {
        /* Windows drive letters (C:/...) land here: not a registry URL. */
        forge_util_set_error(error, error_size,
                  "registry URL '%s' has no usable transport prefix "
                  "(https://, http://localhost, or file:// with "
                  "FORGE_ALLOW_UNSAFE_REGISTRY=1)", url);
        return -1;
    }
    if (scheme_length == 5U && scheme_matches(url, scheme_length, "https")) {
        return 0;
    }
    if (scheme_length == 4U && scheme_matches(url, scheme_length, "http")) {
        const char *host = colon + 1U;
        const char *end;
        size_t host_length;

        if (host[0] != '/' || host[1] != '/') {
            forge_util_set_error(error, error_size,
                      "registry URL '%s' is not a usable http reference", url);
            return -1;
        }
        host += 2U;
        end = host + strcspn(host, "/?#:");
        host_length = (size_t)(end - host);
        if (host_is_loopback(host, host_length)) {
            return 0;
        }
        forge_util_set_error(error, error_size,
                  "registry URL '%s' uses plain http outside localhost; "
                  "serve the registry over https", url);
        return -1;
    }
    if (scheme_length == 4U && scheme_matches(url, scheme_length, "file")) {
        const char *override = getenv("FORGE_ALLOW_UNSAFE_REGISTRY");

        if (override != NULL && override[0] != '\0' && strcmp(override, "0") != 0) {
            return 0;
        }
        forge_util_set_error(error, error_size,
                  "registry URL '%s' uses file://, which reads arbitrary "
                  "local paths; set FORGE_ALLOW_UNSAFE_REGISTRY=1 to allow "
                  "local file registries", url);
        return -1;
    }
    forge_util_set_error(error, error_size,
              "registry URL '%s' uses the '%.*s:' transport, which forge "
              "does not allow; use https:// (http:// only for localhost, "
              "file:// only with FORGE_ALLOW_UNSAFE_REGISTRY=1)",
              url, (int)scheme_length, url);
    return -1;
}

/* Joins one curl invocation: curl --fail --silent --show-error --location
 * --proto =https,http,file --proto-redir =https,http --max-time 120
 * --output <dest> -- <url>. The -- handoff keeps exotic URLs out of the
 * option parser even though the policy above already refused '-' URLs. */
static int download_with_curl(ForgeLogger *logger, const char *url,
                              const char *dest_path, char *error,
                              size_t error_size)
{
    ForgeArgv argv = {0};
    int status;

    if (forge_argv_append(&argv, "curl") != 0 ||
        forge_argv_append(&argv, "--fail") != 0 ||
        forge_argv_append(&argv, "--silent") != 0 ||
        forge_argv_append(&argv, "--show-error") != 0 ||
        forge_argv_append(&argv, "--location") != 0 ||
        forge_argv_append(&argv, "--proto") != 0 ||
        forge_argv_append(&argv, "=https,http,file") != 0 ||
        forge_argv_append(&argv, "--proto-redir") != 0 ||
        forge_argv_append(&argv, "=https,http") != 0 ||
        forge_argv_append(&argv, "--max-time") != 0 ||
        forge_argv_append(&argv, "120") != 0 ||
        forge_argv_append(&argv, "--output") != 0 ||
        forge_argv_append(&argv, dest_path) != 0 ||
        forge_argv_append(&argv, "--") != 0 ||
        forge_argv_append(&argv, url) != 0) {
        forge_argv_free(&argv);
        forge_util_set_error(error, error_size,
                  "out of memory while building a curl command");
        return -1;
    }
    forge_logger_detail(logger, "deps", "downloading %s", url);
    status = run_tool(logger, "curl", &argv, NULL, error, error_size);
    forge_argv_free(&argv);
    return status;
}

#if FORGE_PLATFORM_WINDOWS
/* Last resort on Windows when curl is absent: PowerShell's
 * Invoke-WebRequest, still argv-spawned (no shell, no interpolation). */
static int download_with_powershell(ForgeLogger *logger, const char *url,
                                    const char *dest_path, char *error,
                                    size_t error_size)
{
    ForgeArgv argv = {0};
    int status;

    if (forge_argv_append(&argv, "powershell") != 0 ||
        forge_argv_append(&argv, "-NoProfile") != 0 ||
        forge_argv_append(&argv, "-NonInteractive") != 0 ||
        forge_argv_append(&argv, "-ExecutionPolicy") != 0 ||
        forge_argv_append(&argv, "Bypass") != 0 ||
        forge_argv_append(&argv, "-Command") != 0 ||
        forge_argv_append(&argv, "Invoke-WebRequest") != 0 ||
        forge_argv_append(&argv, "-Uri") != 0 ||
        forge_argv_append(&argv, url) != 0 ||
        forge_argv_append(&argv, "-OutFile") != 0 ||
        forge_argv_append(&argv, dest_path) != 0 ||
        forge_argv_append(&argv, "-UseBasicParsing") != 0) {
        forge_argv_free(&argv);
        forge_util_set_error(error, error_size,
                  "out of memory while building a PowerShell download");
        return -1;
    }
    forge_logger_detail(logger, "deps", "downloading %s", url);
    status = run_tool(logger, "powershell", &argv, NULL, error, error_size);
    forge_argv_free(&argv);
    return status;
}
#endif

int forge_fetch_to_file(ForgeLogger *logger, const char *url,
                        const char *dest_path, char *error, size_t error_size)
{
    if (url == NULL || dest_path == NULL) {
        forge_util_set_error(error, error_size, "download needs a URL and a destination");
        return -1;
    }
    if (forge_util_program_available("curl")) {
        return download_with_curl(logger, url, dest_path, error, error_size);
    }
#if FORGE_PLATFORM_WINDOWS
    if (forge_util_program_available("powershell")) {
        return download_with_powershell(logger, url, dest_path, error,
                                        error_size);
    }
#endif
    forge_util_set_error(error, error_size,
              "curl was not found on PATH; registry downloads require it");
    return -1;
}

/* Tar-slip guard: registry tarballs are fetched from the network, so every
 * member must stay inside the destination directory. One `tar -t` line per
 * member: relative, no drive/UNC prefix, no backslash separators (ambiguous
 * on Windows), and no ".." segment. Anything else fails the unpack before
 * anything is extracted. */
static int tar_member_is_safe(const char *member)
{
    const char *segment;
    size_t length;

    if (member == NULL || member[0] == '\0') {
        return 0;
    }
    if (member[0] == '/' || member[0] == '\\') {
        return 0;
    }
    if (((member[0] >= 'A' && member[0] <= 'Z') ||
         (member[0] >= 'a' && member[0] <= 'z')) && member[1] == ':') {
        return 0;
    }
    for (segment = member; *segment != '\0'; ++segment) {
        if (*segment == '\\') {
            return 0;
        }
    }
    for (segment = member;;) {
        const char *slash = strchr(segment, '/');

        length = slash == NULL ? strlen(segment) : (size_t)(slash - segment);
        if (length == 2U && segment[0] == '.' && segment[1] == '.') {
            return 0;
        }
        if (slash == NULL) {
            return 1;
        }
        segment = slash + 1U;
    }
}

/* Splits `path` into its containing directory and bare file name. GNU tar
 * parses an absolute Windows path ("C:/..." or "C:\...") as a remote
 * "host:file", so tar always runs with the archive's directory as its
 * working directory and the bare name as its operand: that spelling is
 * local on every tar (GNU, bsdtar) on every platform. The directory keeps
 * its original separators (child cwd resolution is a native API call, not
 * tar's host:file parser). */
static void split_archive_path(const char *path, char *dir_out,
                               size_t dir_size, const char **base_out)
{
    const char *slash = strrchr(path, '/');
    const char *backslash = strrchr(path, '\\');
    const char *base = slash;
    size_t length;

    if (backslash != NULL && (base == NULL || backslash > base)) {
        base = backslash;
    }
    if (base == NULL) {
        if (dir_size != 0U) {
            dir_out[0] = '\0';
        }
        *base_out = path;
        return;
    }
    length = (size_t)(base - path);
    if (length >= dir_size) {
        length = dir_size - 1U;
    }
    if (dir_size != 0U) {
        memcpy(dir_out, path, length);
        dir_out[length] = '\0';
        if (dir_out[0] == '\0') {
            dir_out[0] = '.';
            dir_out[1] = '\0';
        }
    }
    *base_out = base + 1U;
}

/* Runs `tar -t[z]f <archive>` (plus -v for the type listing), capturing the
 * listing to `list_path` for validation. The child runs in the archive's
 * directory with the bare file name (see split_archive_path); `list_path`
 * stays absolute since redirection is a native call. */
static int tar_capture_listing(ForgeLogger *logger, const char *archive,
                               const char *list_path, int verbose,
                               char *error, size_t error_size)
{
    ForgeArgv argv = {0};
    char archive_dir[FORGE_PATH_MAX];
    const char *archive_base;
    int exit_code = 0;
    int status;

    (void)logger;
    split_archive_path(archive, archive_dir, sizeof(archive_dir),
                       &archive_base);
    if (forge_argv_append(&argv, "tar") != 0 ||
        forge_argv_append(&argv, verbose ? "-tvzf" : "-tzf") != 0 ||
        forge_argv_append(&argv, archive_base) != 0) {
        forge_argv_free(&argv);
        forge_util_set_error(error, error_size,
                  "out of memory while building a tar command");
        return -1;
    }
    if (forge_argv_finalize(&argv) != 0) {
        forge_argv_free(&argv);
        forge_util_set_error(error, error_size,
                  "out of memory while building a tar command");
        return -1;
    }
    status = forge_process_run_at(archive_dir[0] == '\0' ? NULL : archive_dir,
                                argv.items, list_path, 0, &exit_code, error,
                                error_size);
    forge_argv_free(&argv);
    if (status != 0) {
        return -1;
    }
    if (exit_code != 0) {
        forge_util_set_error(error, error_size,
                  "tar failed to list '%s' (exit %d); refusing to unpack",
                  archive, exit_code);
        return -1;
    }
    return 0;
}

#define FORGE_TAR_LIST_MAX_LINES 65536U

/* Reads one captured line; returns 1 with the newline stripped, 0 at EOF,
 * -1 on an overlong line or I/O error. */
static int read_listing_line(FILE *stream, char *line, size_t line_size,
                             char *error, size_t error_size)
{
    size_t length;

    if (fgets(line, (int)line_size, stream) == NULL) {
        if (ferror(stream)) {
            forge_util_set_error(error, error_size,
                      "could not read tar listing");
            return -1;
        }
        return 0;
    }
    length = strlen(line);
    if (length == line_size - 1U && line[length - 1U] != '\n') {
        forge_util_set_error(error, error_size,
                  "tar listing has an overlong entry; refusing to unpack");
        return -1;
    }
    while (length != 0U && (line[length - 1U] == '\n' || line[length - 1U] == '\r')) {
        line[--length] = '\0';
    }
    return 1;
}

static int validate_member_listing(const char *list_path, const char *archive,
                                   char *error, size_t error_size)
{
    FILE *stream;
    char line[FORGE_PATH_MAX + 2U];
    unsigned long count = 0UL;
    int status;

    stream = fopen(list_path, "r");
    if (stream == NULL) {
        forge_util_set_error(error, error_size,
                  "could not read tar listing for '%s'", archive);
        return -1;
    }
    for (;;) {
        status = read_listing_line(stream, line, sizeof(line), error, error_size);
        if (status <= 0) {
            break;
        }
        if (++count > FORGE_TAR_LIST_MAX_LINES) {
            forge_util_set_error(error, error_size,
                      "tar archive '%s' lists too many members; refusing to unpack",
                      archive);
            (void)fclose(stream);
            return -1;
        }
        if (!tar_member_is_safe(line)) {
            forge_util_set_error(error, error_size,
                      "tar archive '%s' has an unsafe member '%s'; refusing to unpack",
                      archive, line);
            (void)fclose(stream);
            return -1;
        }
    }
    if (fclose(stream) != 0 || status < 0) {
        if (status >= 0) {
            forge_util_set_error(error, error_size,
                      "could not read tar listing for '%s'", archive);
        }
        return -1;
    }
    return 0;
}

/* The verbose listing starts every line with the entry type: '-' for a
 * regular file, 'd' for a directory. Symlinks ('l'), hard links ('h'), and
 * device nodes/fifos/sockets would let a hostile archive write outside the
 * destination (or plant blocking nodes), so anything else fails closed.
 * Both GNU tar and bsdtar (Windows/macOS) use these leading letters. */
static int validate_type_listing(const char *list_path, const char *archive,
                                 char *error, size_t error_size)
{
    FILE *stream;
    char line[FORGE_PATH_MAX + 2U];
    int status;

    stream = fopen(list_path, "r");
    if (stream == NULL) {
        forge_util_set_error(error, error_size,
                  "could not read tar listing for '%s'", archive);
        return -1;
    }
    for (;;) {
        status = read_listing_line(stream, line, sizeof(line), error, error_size);
        if (status <= 0) {
            break;
        }
        if (line[0] != '-' && line[0] != 'd') {
            forge_util_set_error(error, error_size,
                      "tar archive '%s' has a non-regular entry '%.64s'; refusing to unpack",
                      archive, line);
            (void)fclose(stream);
            return -1;
        }
    }
    if (fclose(stream) != 0 || status < 0) {
        if (status >= 0) {
            forge_util_set_error(error, error_size,
                      "could not read tar listing for '%s'", archive);
        }
        return -1;
    }
    return 0;
}

int forge_fetch_unpack_tar_gz(ForgeLogger *logger, const char *archive,
                              const char *dest_dir, char *error,
                              size_t error_size)
{
    ForgeArgv argv = {0};
    char members_path[FORGE_PATH_MAX];
    char types_path[FORGE_PATH_MAX];
    char archive_dir[FORGE_PATH_MAX];
    const char *archive_base;
    int status;

    if (archive == NULL || dest_dir == NULL) {
        forge_util_set_error(error, error_size, "unpack needs an archive and a directory");
        return -1;
    }
    if (!forge_util_program_available("tar")) {
        forge_util_set_error(error, error_size,
                  "tar was not found on PATH; registry packages ship as .tar.gz");
        return -1;
    }
    if (forge_paths_ensure_directory(dest_dir, error, error_size) != 0) {
        return -1;
    }
    /* Validate member names and entry types before extracting anything:
     * a hostile tarball must not yield absolute paths, ".." escapes, or
     * links/nodes that write outside the destination (tar-slip). */
    if ((size_t)snprintf(members_path, sizeof(members_path), "%s.forge-members.tmp",
                         archive) >= sizeof(members_path) ||
        (size_t)snprintf(types_path, sizeof(types_path), "%s.forge-types.tmp",
                         archive) >= sizeof(types_path)) {
        forge_util_set_error(error, error_size, "archive path is too long");
        return -1;
    }
    if (tar_capture_listing(logger, archive, members_path, 0, error, error_size) != 0 ||
        validate_member_listing(members_path, archive, error, error_size) != 0) {
        (void)remove(members_path);
        (void)remove(types_path);
        return -1;
    }
    if (tar_capture_listing(logger, archive, types_path, 1, error, error_size) != 0 ||
        validate_type_listing(types_path, archive, error, error_size) != 0) {
        (void)remove(members_path);
        (void)remove(types_path);
        return -1;
    }
    (void)remove(members_path);
    (void)remove(types_path);
    split_archive_path(archive, archive_dir, sizeof(archive_dir),
                       &archive_base);
    if (forge_argv_append(&argv, "tar") != 0 ||
        forge_argv_append(&argv, "-xzf") != 0 ||
        forge_argv_append(&argv, archive_base) != 0 ||
        forge_argv_append(&argv, "-C") != 0 ||
        forge_argv_append(&argv, dest_dir) != 0) {
        forge_argv_free(&argv);
        forge_util_set_error(error, error_size,
                  "out of memory while building a tar command");
        return -1;
    }
    forge_logger_detail(logger, "deps", "unpacking %s", archive);
    status = run_tool(logger, "tar", &argv,
                      archive_dir[0] == '\0' ? NULL : archive_dir, error,
                      error_size);
    forge_argv_free(&argv);
    return status;
}
