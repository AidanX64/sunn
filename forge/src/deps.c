#include "forge/platform.h"

#if !FORGE_PLATFORM_WINDOWS
#define _XOPEN_SOURCE 700
#define _POSIX_C_SOURCE 200809L
#endif

#include <ctype.h>
#include <errno.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#if FORGE_PLATFORM_WINDOWS
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <fcntl.h>
#include <io.h>
#else
#include <fcntl.h>
#include <unistd.h>
#include <dirent.h>
#endif
#include <sys/stat.h>
#include <sys/types.h>

#include "forge/argv.h"
#include "forge/deps.h"
#include "forge/process.h"
#include "forge/registry.h"
#include "forge/thread.h"
#include "forge_util.h"

#define FORGE_DEPS_VALUE_MAX FORGE_MANIFEST_VALUE_MAX
#define FORGE_LOCK_MAX_ENTRIES FORGE_MANIFEST_MAX_DEPS
#define FORGE_SCAN_ENTRY_LIMIT 20000U
#define FORGE_SCAN_MAX_DEPTH 64U
#define FORGE_LOCK_LINE_MAX 4096U

/* One pinned dependency as recorded in Forge.lock. Plain git entries retain
 * their historical shape; registry entries carry an explicit source kind.
 * Registry revision 0 doubles as "predates revisions" for old lockfiles;
 * features default to "" likewise. */
typedef struct ForgeLockEntry {
    char name[FORGE_DEPS_VALUE_MAX];
    char commit[FORGE_DEPS_VALUE_MAX];
    char url[FORGE_PATH_MAX];
    char ref[FORGE_DEPS_VALUE_MAX];
    char version[FORGE_DEPS_VALUE_MAX];
    char sha256[FORGE_DEPS_VALUE_MAX];
    char kind[8];
    char location[FORGE_PATH_MAX];
    unsigned revision;
    char features[FORGE_FEATURES_JOINED_MAX];
} ForgeLockEntry;

typedef struct ForgeLockFile {
    ForgeLockEntry items[FORGE_LOCK_MAX_ENTRIES];
    size_t count;
    int exists;
} ForgeLockFile;

/* ------------------------------------------------------------------ */
/* Small helpers                                                       */
/* ------------------------------------------------------------------ */

static void deps_log(ForgeLogger *logger, const char *stage, const char *format, ...)
{
    va_list arguments;
    char message[FORGE_COMMAND_MAX];

    va_start(arguments, format);
    (void)vsnprintf(message, sizeof(message), format, arguments);
    va_end(arguments);
    if (logger != NULL) {
        forge_logger_log(logger, stage, "%s", message);
    } else {
        fprintf(stderr, "forge: [%s] %s\n", stage, message);
    }
}

/* The shared dependency cache root: $FORGE_HOME or ~/.forge. */
static int forge_home_root(char *destination, size_t destination_size)
{
    const char *override = getenv("FORGE_HOME");
    const char *home = getenv("HOME");

#if FORGE_PLATFORM_WINDOWS
    if (home == NULL || home[0] == '\0') {
        home = getenv("USERPROFILE");
    }
#endif
    if (override != NULL && override[0] != '\0') {
        return snprintf(destination, destination_size, "%s", override) >= 0 &&
               strlen(override) < destination_size ? 0 : -1;
    }
    if (home == NULL || home[0] == '\0') {
        return -1;
    }
    return snprintf(destination, destination_size, "%s/.forge", home) >= 0 &&
           strlen(home) + 7U < destination_size ? 0 : -1;
}

int forge_deps_cache_home(char *destination, size_t destination_size)
{
    if (destination == NULL || destination_size == 0U) {
        return -1;
    }
    return forge_home_root(destination, destination_size);
}

/* FNV-1a over the URL keeps cache directories unique per source. */
static void url_cache_hash(const char *url, char *hex, size_t hex_size)
{
    unsigned long long hash = 1469598103934665603ULL;
    const char *cursor;

    for (cursor = url; *cursor != '\0'; ++cursor) {
        hash ^= (unsigned char)*cursor;
        hash *= 1099511628211ULL;
    }
    (void)snprintf(hex, hex_size, "%016llx", (unsigned long long)hash);
}

static int file_exists(const char *path)
{
    struct stat details;

    return stat(path, &details) == 0 && S_ISREG(details.st_mode);
}

static int directory_exists(const char *path)
{
    struct stat details;

    return stat(path, &details) == 0 && S_ISDIR(details.st_mode);
}

/* True when anything (file or directory) exists at `path`. */
static int path_exists(const char *path)
{
    struct stat details;

    return stat(path, &details) == 0;
}

/* Joins dir + name and reports whether the result exists at all. */
static int dir_has_entry(const char *dir, const char *name)
{
    char path[FORGE_PATH_MAX];

    return forge_paths_join(path, sizeof(path), dir, name) == 0 &&
           path_exists(path);
}

/* Joins dir + name and reports whether the result is a regular file. */
static int dir_has_file(const char *dir, const char *name)
{
    char path[FORGE_PATH_MAX];

    return forge_paths_join(path, sizeof(path), dir, name) == 0 &&
           file_exists(path);
}

static void deps_sleep_ms(unsigned milliseconds)
{
#if FORGE_PLATFORM_WINDOWS
    Sleep(milliseconds);
#else
    struct timespec pause;

    pause.tv_sec = (time_t)(milliseconds / 1000U);
    pause.tv_nsec = (long)(milliseconds % 1000U) * 1000000L;
    nanosleep(&pause, NULL);
#endif
}

/* Age of `path` in seconds, or -1 when it cannot be stat'ed. */
static double path_age_seconds(const char *path)
{
    struct stat details;
    time_t now = time(NULL);

    if (stat(path, &details) != 0 || details.st_mtime > now) {
        return -1.0;
    }
    return (double)(now - details.st_mtime);
}

/* Runs a program directly (no shell), echoing the command line through the
 * logger and capturing output into the invocation log like other stages. */
static int run_tool(ForgeLogger *logger, const char *stage, ForgeArgv *argv,
                    const char *capture_path, int truncating_capture,
                    int *exit_code, char *error, size_t error_size)
{
    char display[FORGE_COMMAND_MAX];

    if (forge_argv_join(display, sizeof(display), argv) == 0 && logger != NULL) {
        forge_logger_log(logger, stage, "command: %s", display);
    }
    (void)forge_argv_finalize(argv);
    return forge_process_run(argv->items, capture_path, truncating_capture ? 0 : 1,
                             exit_code, error, error_size);
}

/* ------------------------------------------------------------------ */
/* Git URL policy                                                      */
/* ------------------------------------------------------------------ */

/*
 * FORGE_ALLOW_UNSAFE_GIT lifts the URL restriction below for local and
 * air-gapped testing (plain paths, file://, ...). Never enable it when
 * resolving manifests you do not control.
 */
static int env_allows_unsafe_git(void)
{
    const char *override = getenv("FORGE_ALLOW_UNSAFE_GIT");

    return override != NULL && override[0] != '\0' && strcmp(override, "0") != 0;
}

static int is_scheme_character(unsigned char character, int first)
{
    if ((character >= 'a' && character <= 'z') ||
        (character >= 'A' && character <= 'Z')) {
        return 1;
    }
    if (first) {
        return 0;
    }
    return (character >= '0' && character <= '9') || character == '+' ||
           character == '-' || character == '.';
}

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

/*
 * Manifest-supplied URLs go onto the `git clone` command line verbatim, so
 * everything git accepts would implicitly be accepted here too — including
 * ext::/fd:: pseudo-transports that execute arbitrary shell commands and
 * option-shaped strings such as "--upload-pack=<program>". Forge therefore
 * allowlists exactly the transports a source dependency plausibly uses:
 *
 *   https://host/path         ordinary public repositories
 *   ssh://[user@]host/path    authenticated clones over SSH
 *   [user@]host.tld:path      scp-style shorthand for ssh://
 *
 * Everything else is refused loudly rather than handed to git.
 */
/* Validates the authority section of an https:// or ssh:// URL so a crafted
 * host cannot turn into an ssh option: `ssh://-oProxyCommand=.../x` would
 * otherwise reach `git clone` (a lone argv element, so no shell is involved)
 * and git would hand the `-o...` host to ssh as a command-line flag.
 * Grammar: [user@]host[:port], host non-empty, never starting with '-'. */
static int url_authority_is_safe(const char *url, size_t scheme_length)
{
    const char *cursor = url + scheme_length;
    const char *host;
    const char *end;

    /* Skip "://". */
    if (cursor[0] != ':' || cursor[1] != '/' || cursor[2] != '/') {
        return 0;
    }
    cursor += 3U;
    /* Strip optional userinfo: [user@]. */
    for (end = cursor; *end != '\0' && *end != '/' && *end != '?' && *end != '#'; ++end) {
        if (*end == '@') {
            const char *user;

            for (user = cursor; user < end; ++user) {
                unsigned char c = (unsigned char)*user;

                if (!(isalnum(c) || c == '.' || c == '-' || c == '_' ||
                      c == '~' || c == '+')) {
                    return 0;
                }
            }
            cursor = end + 1;
            break;
        }
    }
    host = cursor;
    if (host[0] == '[') {
        /* Bracketed IPv6 literal: [::1], optionally followed by :port. */
        const char *close = strchr(host, ']');
        const char *port;

        if (close == NULL || close == host + 1U) {
            return 0;
        }
        for (cursor = host + 1U; cursor < close; ++cursor) {
            unsigned char c = (unsigned char)*cursor;

            if (!(isxdigit(c) || c == ':' || c == '.')) {
                return 0;
            }
        }
        if (*close == ']' && close[1] != ':' && close[1] != '\0' &&
            close[1] != '/' && close[1] != '?' && close[1] != '#') {
            return 0;
        }
        if (close[1] != ':') {
            return 1;
        }
        port = close + 2U;
        if (*port == '\0') {
            return 0;
        }
        for (; *port != '\0' && *port != '/' && *port != '?' && *port != '#'; ++port) {
            if (!isdigit((unsigned char)*port)) {
                return 0;
            }
        }
        return 1;
    }
    for (end = host; *end != '\0' && *end != '/' && *end != '?' && *end != '#'; ++end) {
        if (*end == ':') {
            /* Port: digits only, non-empty. */
            const char *port = end + 1;

            if (*port == '\0') {
                return 0;
            }
            for (; *port != '\0' && *port != '/' && *port != '?' && *port != '#'; ++port) {
                if (!isdigit((unsigned char)*port)) {
                    return 0;
                }
            }
            break;
        }
    }
    if (end == host || host[0] == '-') {
        return 0;
    }
    for (; host < end; ++host) {
        unsigned char c = (unsigned char)*host;

        if (!(isalnum(c) || c == '.' || c == '-' || c == '_')) {
            return 0;
        }
    }
    return 1;
}

int forge_deps_git_url_is_supported(const char *url, char *error, size_t error_size)
{
    static const char *const rejection =
        "use https://host/path, ssh://host/path, or git@host:path "
        "(set FORGE_ALLOW_UNSAFE_GIT=1 to override)";
    const char *cursor;
    const char *colon;
    size_t scheme_length;
    int looks_like_scheme;

    if (env_allows_unsafe_git()) {
        return 0;
    }
    if (url == NULL || url[0] == '\0') {
        forge_util_set_error(error, error_size, "git dependency has an empty URL");
        return -1;
    }
    if (url[0] == '-') {
        forge_util_set_error(error, error_size,
                  "git URL '%s' starts with '-' and would be parsed by git as "
                  "an option instead of a location", url);
        return -1;
    }
    for (cursor = url; *cursor != '\0'; ++cursor) {
        unsigned char character = (unsigned char)*cursor;

        if (character < 0x20 || character == 0x7F) {
            forge_util_set_error(error, error_size,
                      "git URL '%s' contains control characters", url);
            return -1;
        }
    }
    colon = strchr(url, ':');
    if (colon == NULL) {
        forge_util_set_error(error, error_size,
                  "git URL '%s' is not supported; %s", url, rejection);
        return -1;
    }
    scheme_length = (size_t)(colon - url);
    looks_like_scheme = is_scheme_character((unsigned char)url[0], 1);
    for (cursor = url + 1; looks_like_scheme && cursor < colon; ++cursor) {
        looks_like_scheme = is_scheme_character((unsigned char)*cursor, 0);
    }
    if (looks_like_scheme) {
        /* Any well-formed transport prefix other than the two below is
         * refused: ext:: and fd:: execute commands, file:// and plain
         * schemes bypass the cache policy, http:// clones insecurely. */
        if ((scheme_length == 5U && scheme_matches(url, scheme_length, "https")) ||
            (scheme_length == 3U && scheme_matches(url, scheme_length, "ssh"))) {
            if (!url_authority_is_safe(url, scheme_length)) {
                forge_util_set_error(error, error_size,
                          "git URL '%s' has an unusable host; %s", url,
                          rejection);
                return -1;
            }
            return 0;
        }
        forge_util_set_error(error, error_size,
                  "git URL '%s' uses the '%.*s:' transport, which forge does "
                  "not allow; %s", url, (int)scheme_length, url, rejection);
        return -1;
    }
    /* scp-style [user@]host:path: the piece before the ':' must actually
     * look like a host (this also rejects Windows drive letters like C:).
     * A host without any letter cannot be one. */
    {
        const char *host_start = url;

        for (cursor = url; cursor < colon; ++cursor) {
            if (*cursor == '@') {
                host_start = cursor + 1;
            }
        }
        if (host_start == colon || colon[1] == '\0' || colon[1] == ':') {
            forge_util_set_error(error, error_size,
                      "git URL '%s' is not a usable git@host:path reference; %s",
                      url, rejection);
            return -1;
        }
        for (cursor = host_start; cursor < colon; ++cursor) {
            unsigned char character = (unsigned char)*cursor;

            if (!(isalpha(character) || character == '.' || character == '-' ||
                  character == '_' || character == '[' || character == ']')) {
                forge_util_set_error(error, error_size,
                          "git URL '%s' is not a usable git@host:path "
                          "reference; %s", url, rejection);
                return -1;
            }
        }
    }
    return 0;
}

/* Ref names and locked SHAs also travel as lone argv elements; refuse
 * option-shaped ones so neither a manifest nor a tampered lockfile can
 * smuggle extra flags into `git checkout`. */
static int checkout_target_is_safe(const char *target, const char *what,
                                   const char *dependency_name,
                                   char *error, size_t error_size)
{
    if (target[0] == '-') {
        forge_util_set_error(error, error_size,
                  "dependency '%s': %s '%s' is not a valid git reference",
                  dependency_name, what, target);
        return -1;
    }
    return 0;
}

/* ------------------------------------------------------------------ */
/* Git operations (via the git CLI; no library dependency)             */
/* ------------------------------------------------------------------ */

static int git_available(char *error, size_t error_size)
{
    if (!forge_util_program_available("git")) {
        forge_util_set_error(error, error_size,
                  "git was not found on PATH; git dependencies require it");
        return -1;
    }
    return 0;
}

/*
 * Runs `git` with `arguments` (NULL-terminated, without the program name).
 * When `repo_dir` is non-NULL the command is scoped to that repository via
 * the global -C flag, since forge's own working directory is unrelated.
 */
static int git_run(ForgeLogger *logger, const char *repo_dir, char **arguments,
                   const char *capture_path, int truncating_capture,
                   char *error, size_t error_size)
{
    ForgeArgv argv = {0};
    int exit_code = 0;
    int status;

    if (forge_argv_append(&argv, "git") != 0 ||
        (repo_dir != NULL &&
         (forge_argv_append(&argv, "-C") != 0 ||
          forge_argv_append(&argv, repo_dir) != 0))) {
        goto out_of_memory;
    }
    for (char **cursor = arguments; *cursor != NULL; ++cursor) {
        if (forge_argv_append(&argv, *cursor) != 0) {
            goto out_of_memory;
        }
    }
    status = run_tool(logger, "deps", &argv, capture_path, truncating_capture,
                      &exit_code, error, error_size);
    forge_argv_free(&argv);
    if (status != 0) {
        return -1;
    }
    if (exit_code != 0) {
        forge_util_set_error(error, error_size, "git %s failed (exit %d)",
                  arguments[0], exit_code);
        return -1;
    }
    return 0;
out_of_memory:
    forge_argv_free(&argv);
    forge_util_set_error(error, error_size, "out of memory while building a git command");
    return -1;
}

/* Reads the first line of a file (used to capture `git rev-parse` output). */
static int read_first_line(const char *path, char *output, size_t output_size)
{
    FILE *file = fopen(path, "r");
    char line[FORGE_PATH_MAX];

    if (file == NULL) {
        return -1;
    }
    if (fgets(line, sizeof(line), file) == NULL) {
        (void)fclose(file);
        return -1;
    }
    (void)fclose(file);
    line[strcspn(line, "\r\n")] = '\0';
    if (strlen(line) >= output_size) {
        return -1;
    }
    (void)snprintf(output, output_size, "%s", line);
    return 0;
}

/* Removes the cached clone at `cache_dir` best-effort so a fresh clone can
 * replace it. Used when the directory exists but is not a usable repository
 * (e.g. an interrupted clone left a partial checkout behind). */
static void discard_cache_dir(const char *cache_dir)
{
    forge_paths_remove_tree(cache_dir, NULL, 0U);
}

/*
 * Makes sure `cache_dir` holds a clone of `url` with `target` (a ref, or a
 * locked commit SHA) checked out, and reports the resolved commit SHA.
 * When `locked_commit` is non-empty and no update was forced, the fetch is
 * skipped entirely so offline rebuilds stay quiet and fast. With
 * `submodules` set, submodules are cloned/updated alongside every checkout.
 * `offline` (from --offline) forbids every network step: a cached clone with
 * a matching lock pin still checks out locally, anything else fails here,
 * before git can run.
 */
int forge_deps_ensure_git_checkout(ForgeLogger *logger, const char *name,
                                   const char *url, const char *ref,
                                   const char *locked_commit, int force_update,
                                   int submodules, int offline,
                                   const char *cache_dir,
                                   char *resolved_sha, size_t resolved_sha_size,
                                   char *error, size_t error_size)
{
    char capture[FORGE_PATH_MAX];
    char origin_target[FORGE_DEPS_VALUE_MAX + 8U];
    /* Slots: clone --quiet <url> [--recurse-submodules] <dir> NULL. */
    char *clone_arguments[6] = { "clone", "--quiet", (char *)url, (char *)cache_dir, NULL };
    char *fetch_arguments[] = { "fetch", "--quiet", "--tags", "origin", NULL };
    char *checkout_arguments[] = { "checkout", "--quiet", "--detach", NULL, NULL };
    char *submodule_arguments[] = {
        "submodule", "update", "--init", "--recursive", NULL
    };
    char *revparse_arguments[] = { "rev-parse", "HEAD", NULL };
    char target[FORGE_DEPS_VALUE_MAX];
    char *checkout_target = NULL;
    int use_locked = locked_commit[0] != '\0' && !force_update;
    int just_cloned = 0;
    int have_ref;

    if (submodules) {
        clone_arguments[3] = "--recurse-submodules";
        clone_arguments[4] = (char *)cache_dir;
        clone_arguments[5] = NULL;
    }

    if (checkout_target_is_safe(ref, "ref", name, error, error_size) != 0 ||
        (use_locked && checkout_target_is_safe(locked_commit, "locked commit",
                                               name, error, error_size) != 0)) {
        return -1;
    }
    have_ref = ref[0] != '\0';

    /*
     * The offline contract: only the fully-local path survives. That needs a
     * warm cache AND a lock pin matching the manifest's url/ref — anything
     * else would require clone/fetch, which --offline refuses up front so
     * the message names the policy instead of surfacing a git network error.
     */
    if (offline && !use_locked) {
        forge_util_set_error(error, error_size,
                  "dependency '%s' cannot be resolved offline; it has no lock "
                  "pin matching the manifest (or an update was forced). "
                  "Rerun without --offline once to fetch it",
                  name);
        return -1;
    }

    /*
     * A directory without a .git entry is not a usable clone (an interrupted
     * clone can leave one behind); wipe it so the clone below starts clean
     * instead of failing forever with "not a git repository". Both layouts
     * are accepted: normal clones have a .git directory, worktrees a file.
     */
    if (directory_exists(cache_dir) && !dir_has_entry(cache_dir, ".git")) {
        deps_log(logger, "deps", "dependency cache for %s is incomplete; re-cloning",
                 name);
        discard_cache_dir(cache_dir);
    }
    if (!directory_exists(cache_dir)) {
        deps_log(logger, "deps", "cloning %s (%s)", name, url);
        if (git_run(logger, NULL, clone_arguments, NULL, 0, error, error_size) != 0) {
            /* A failed clone often leaves a partial directory; clear it so the
             * next attempt starts from scratch instead of seeing a broken repo. */
            discard_cache_dir(cache_dir);
            return -1;
        }
        just_cloned = 1;
    } else if (!use_locked) {
        if (git_run(logger, cache_dir, fetch_arguments, NULL, 0, error, error_size) != 0) {
            return -1;
        }
    }

    /*
     * Reconcile HEAD with what the caller asked for — including right after
     * a fresh clone. A cold clone lands on the remote's default-branch tip,
     * but that is only correct when the manifest tracks that tip: honoring
     * the lock pin and manifest ref here keeps a moved default branch from
     * silently bypassing Forge.lock on fresh machines and CI runners.
     */
    if (use_locked) {
        (void)snprintf(target, sizeof(target), "%s", locked_commit);
        checkout_target = target;
    } else if (have_ref) {
        /*
         * `fetch` only advances the remote-tracking refs; a local branch of
         * the same name keeps its old commit, so checking out the bare name
         * after a fetch would silently resolve a forced update right back
         * onto the stale pin. Prefer the fresh remote-tracking ref once a
         * fetch ran; on a cold clone nothing is stale, and the plain name
         * (a branch may not exist locally yet) goes first. Whichever form
         * fails, the other is retried below — tags and raw SHAs never exist
         * under origin/, while brand-new branches never exist locally.
         */
        (void)snprintf(target, sizeof(target), "%s", ref);
        (void)snprintf(origin_target, sizeof(origin_target), "origin/%s", ref);
        checkout_target = just_cloned ? target : origin_target;
    } else if (!just_cloned) {
        /*
         * No pin and no ref: track the remote's default branch tip.
         * FETCH_HEAD only exists once a fetch ran, which is why a
         * just-cloned repository skips this case entirely — it already
         * sits exactly there.
         */
        (void)snprintf(target, sizeof(target), "FETCH_HEAD");
        checkout_target = target;
    }

    if (checkout_target != NULL) {
        const char *retry_target = NULL;

        if (!use_locked && have_ref) {
            retry_target = checkout_target == target ? origin_target : target;
        }
        checkout_arguments[3] = checkout_target;
        if (git_run(logger, cache_dir, checkout_arguments, NULL, 0, error,
                    error_size) != 0) {
            int recovered = 0;

            if (use_locked) {
                /*
                 * The locked commit may simply not exist in this local clone yet
                 * (the cache was cloned while a different ref was checked out).
                 * Fetch once and retry before giving up so offline-capable
                 * rebuilds still recover online — but only when network is
                 * allowed; --offline reports the missing commit instead.
                 */
                if (offline) {
                    forge_util_set_error(error, error_size,
                              "dependency '%s': locked commit %s is not in the "
                              "local cache and --offline forbids fetching it",
                              name, locked_commit);
                    return -1;
                }
                recovered = git_run(logger, cache_dir, fetch_arguments, NULL, 0,
                                    error, error_size) == 0 &&
                            git_run(logger, cache_dir, checkout_arguments, NULL, 0,
                                    error, error_size) == 0;
            } else if (retry_target != NULL) {
                checkout_arguments[3] = (char *)retry_target;
                recovered = git_run(logger, cache_dir, checkout_arguments, NULL, 0,
                                    error, error_size) == 0;
            }
            if (!recovered) {
                return -1;
            }
        }
    }
    if (submodules) {
        /*
         * A detached checkout does not touch submodule working trees, and a
         * moved pin can change which submodule commits are needed — sync
         * them on every checkout, not only after a fresh clone.
         */
        if (git_run(logger, cache_dir, submodule_arguments, NULL, 0, error,
                    error_size) != 0) {
            forge_util_set_error(error, error_size,
                      "dependency '%s': updating git submodules failed", name);
            return -1;
        }
    }
    if (snprintf(capture, sizeof(capture), "%s/revparse.tmp", cache_dir) < 0 ||
        strlen(cache_dir) + 14U >= sizeof(capture)) {
        forge_util_set_error(error, error_size, "dependency cache path is too long");
        return -1;
    }
    if (git_run(logger, cache_dir, revparse_arguments, capture, 1, error, error_size) != 0 ||
        read_first_line(capture, resolved_sha, resolved_sha_size) != 0) {
        forge_util_set_error(error, error_size,
                  "could not read the resolved commit for '%s'", name);
        (void)remove(capture);
        return -1;
    }
    (void)remove(capture);
    return 0;
}

/* ------------------------------------------------------------------ */
/* Lockfile                                                            */
/* ------------------------------------------------------------------ */

static ForgeLockEntry *lock_find(ForgeLockFile *lock, const char *name)
{
    size_t index;

    for (index = 0U; index < lock->count; ++index) {
        if (strcmp(lock->items[index].name, name) == 0) {
            return &lock->items[index];
        }
    }
    return NULL;
}

static int write_lockfile_body(void *user_data, FILE *file)
{
    const ForgeLockFile *lock = user_data;
    size_t index;

    if (fputs("# Generated by forge. Do not edit.\n[dependencies]\n", file) < 0) {
        return -1;
    }
    for (index = 0U; index < lock->count; ++index) {
        const ForgeLockEntry *entry = &lock->items[index];

        if (entry->commit[0] != '\0') {
            if (entry->kind[0] != '\0') {
                if (fprintf(file, "%s = { kind = \"%s\", version = \"%s\", location = \"%s\", ref = \"%s\", commit = \"%s\" }\n",
                            entry->name, entry->kind, entry->version,
                            entry->location, entry->ref, entry->commit) < 0) {
                    return -1;
                }
            } else if (fprintf(file, "%s = { commit = \"%s\", url = \"%s\", ref = \"%s\" }\n",
                        entry->name, entry->commit, entry->url, entry->ref) < 0) {
                return -1;
            }
        } else {
            if (fprintf(file, "%s = { kind = \"%s\", version = \"%s\", location = \"%s\", sha256 = \"%s\", revision = \"%u\", features = \"%s\" }\n",
                        entry->name, entry->kind, entry->version,
                        entry->location, entry->sha256, entry->revision,
                        entry->features) < 0) {
                return -1;
            }
        }
    }
    return 0;
}

/*
 * Parses the generated Forge.lock subset:
 *   [dependencies]
 *   name = { commit = "...", url = "...", ref = "..." }
 */
static int is_full_git_sha(const char *text)
{
    size_t index;

    if (strlen(text) != 40U) {
        return 0;
    }
    for (index = 0U; index < 40U; ++index) {
        unsigned char character = (unsigned char)text[index];

        if (!((character >= '0' && character <= '9') ||
              (character >= 'a' && character <= 'f'))) {
            return 0;
        }
    }
    return 1;
}

/* Registry pins carry a SHA-256 instead of a commit: 64 lowercase hex. */
static int is_full_sha256(const char *text)
{
    size_t index;

    if (text == NULL || strlen(text) != 64U) {
        return 0;
    }
    for (index = 0U; index < 64U; ++index) {
        unsigned char character = (unsigned char)text[index];

        if (!((character >= '0' && character <= '9') ||
              (character >= 'a' && character <= 'f'))) {
            return 0;
        }
    }
    return 1;
}

/*
 * Registry recipe revisions are small decimal integers (shared bound with
 * the registry schema). Absent in old lockfiles, where revision 0 applies.
 */
static int lock_revision_is_valid(const char *text, unsigned *out)
{
    unsigned long value = 0UL;
    size_t index;

    if (text == NULL || text[0] == '\0') {
        return 0;
    }
    for (index = 0U; text[index] != '\0'; ++index) {
        if (!isdigit((unsigned char)text[index])) {
            return 0;
        }
        value = value * 10UL + (unsigned long)(text[index] - '0');
        if (value > FORGE_REGISTRY_MAX_REVISION) {
            return 0;
        }
    }
    *out = (unsigned)value;
    return 1;
}

static int load_lockfile(const char *path, ForgeLockFile *lock,
                         char *error, size_t error_size)
{
    FILE *file;
    char line[FORGE_LOCK_LINE_MAX];
    int in_section = 0;

    memset(lock, 0, sizeof(*lock));
    file = fopen(path, "r");
    if (file == NULL) {
        return 0; /* no lockfile yet is normal */
    }
    lock->exists = 1;
    while (fgets(line, sizeof(line), file) != NULL) {
        char *text = forge_util_trim(line);
        char *equals;
        char *open;
        char *close;
        char *cursor;
        ForgeLockEntry *entry;

        if (*text == '\0' || *text == '#') {
            continue;
        }
        if (*text == '[') {
            in_section = strcmp(text, "[dependencies]") == 0;
            continue;
        }
        if (!in_section) {
            continue;
        }
        equals = strchr(text, '=');
        open = strchr(text, '{');
        close = strrchr(text, '}');
        if (equals == NULL || open == NULL || close == NULL || equals > open) {
            forge_util_set_error(error, error_size,
                      "%s: malformed lock entry '%s'", path, text);
            (void)fclose(file);
            return -1;
        }
        if (lock->count == FORGE_LOCK_MAX_ENTRIES) {
            forge_util_set_error(error, error_size, "%s: too many locked dependencies",
                      path);
            (void)fclose(file);
            return -1;
        }
        entry = &lock->items[lock->count++];
        *equals = '\0';
        text = forge_util_trim(text);
        if (strlen(text) >= sizeof(entry->name)) {
            forge_util_set_error(error, error_size, "%s: dependency name is too long", path);
            (void)fclose(file);
            return -1;
        }
        (void)snprintf(entry->name, sizeof(entry->name), "%s", text);
        cursor = open + 1;
        *close = '\0';
        while (*cursor != '\0') {
            char *key_start;
            char *key_end;
            char *quote;
            char *value;

            /* Skip separators and whitespace between entries. */
            while (*cursor == ',' || isspace((unsigned char)*cursor)) {
                ++cursor;
            }
            if (*cursor == '\0') {
                break;
            }
            key_start = cursor;
            while (*cursor != '\0' && *cursor != '=') {
                ++cursor;
            }
            key_end = cursor;
            if (*cursor == '\0' ||
                memchr(key_start, '"', (size_t)(key_end - key_start)) != NULL) {
                break; /* tolerate trailing commas / whitespace */
            }
            ++cursor; /* skip '=' */
            quote = strchr(cursor, '"');
            if (quote == NULL) {
                break;
            }
            value = quote + 1;
            quote = strchr(value, '"');
            if (quote == NULL) {
                break;
            }
            *key_end = '\0';
            key_start = forge_util_trim(key_start);
            *quote = '\0';
            if (strcmp(key_start, "commit") == 0) {
                (void)snprintf(entry->commit, sizeof(entry->commit), "%s", value);
            } else if (strcmp(key_start, "url") == 0) {
                (void)snprintf(entry->url, sizeof(entry->url), "%s", value);
            } else if (strcmp(key_start, "ref") == 0) {
                (void)snprintf(entry->ref, sizeof(entry->ref), "%s", value);
            } else if (strcmp(key_start, "version") == 0) {
                (void)snprintf(entry->version, sizeof(entry->version), "%s", value);
            } else if (strcmp(key_start, "sha256") == 0) {
                (void)snprintf(entry->sha256, sizeof(entry->sha256), "%s", value);
            } else if (strcmp(key_start, "kind") == 0) {
                (void)snprintf(entry->kind, sizeof(entry->kind), "%s", value);
            } else if (strcmp(key_start, "location") == 0) {
                (void)snprintf(entry->location, sizeof(entry->location), "%s", value);
            } else if (strcmp(key_start, "revision") == 0) {
                if (!lock_revision_is_valid(value, &entry->revision)) {
                    forge_util_set_error(error, error_size,
                              "%s: dependency '%s' has a malformed registry pin; "
                              "delete Forge.lock and run 'forge update' to regenerate it",
                              path, entry->name);
                    (void)fclose(file);
                    return -1;
                }
            } else if (strcmp(key_start, "features") == 0) {
                ForgeDependency scratch;

                /*
                 * Reuse the manifest feature parser so lock spellings
                 * normalize (sorted, deduplicated) exactly like manifest
                 * ones; old lockfiles without the key keep "".
                 */
                memset(&scratch, 0, sizeof(scratch));
                if (forge_parse_feature_list(entry->name[0] != '\0' ?
                                             entry->name : "?",
                                             value, &scratch,
                                             error, error_size) != 0 ||
                    forge_features_join(scratch.feature_count,
                                        scratch.features,
                                        entry->features,
                                        sizeof(entry->features)) != 0) {
                    forge_util_prepend_error(error, error_size, "%s: ", path);
                    (void)fclose(file);
                    return -1;
                }
            }
            cursor = quote + 1;
        }
        if (entry->kind[0] != '\0') {
            if ((strcmp(entry->kind, "git") == 0 &&
                 (entry->version[0] == '\0' || entry->location[0] == '\0' ||
                  entry->ref[0] == '\0' || !is_full_git_sha(entry->commit))) ||
                (strcmp(entry->kind, "url") == 0 &&
                 (entry->version[0] == '\0' || entry->location[0] == '\0' ||
                  !is_full_sha256(entry->sha256) || entry->commit[0] != '\0')) ||
                (strcmp(entry->kind, "git") != 0 && strcmp(entry->kind, "url") != 0)) {
                forge_util_set_error(error, error_size,
                          "%s: dependency '%s' has a malformed registry pin; "
                          "delete Forge.lock and run 'forge update' to regenerate it",
                          path, entry->name);
                (void)fclose(file);
                return -1;
            }
            continue;
        }
        if (entry->commit[0] != '\0') {
            /*
             * S2 gate: the pin is handed to `git checkout --detach` verbatim, and
             * every value forge writes is a full lowercase SHA from rev-parse.
             * Anything else here is a hand-edit or corruption; refuse it loudly
             * instead of checking out whatever git resolves the string to.
             */
            if (!is_full_git_sha(entry->commit)) {
                forge_util_set_error(error, error_size,
                          "%s: dependency '%s' has a malformed commit pin '%s' "
                          "(expected 40 hex digits); delete Forge.lock and run "
                          "'forge update' to regenerate it",
                          path, entry->name,
                          entry->commit[0] != '\0' ? entry->commit : "<missing>");
                (void)fclose(file);
                return -1;
            }
            if (entry->version[0] != '\0' || entry->sha256[0] != '\0') {
                forge_util_set_error(error, error_size,
                          "%s: dependency '%s' mixes git and registry pins; "
                          "delete Forge.lock and run 'forge update' to "
                          "regenerate it",
                          path, entry->name);
                (void)fclose(file);
                return -1;
            }
            continue;
        }
        /*
         * Registry pins mirror the S2 gate: the sha256 is compared against
         * downloaded bytes verbatim, so a hand-edited or corrupt pin must
         * fail here rather than fetching whatever the tampered entry says.
         */
        if (entry->version[0] != '\0' && entry->url[0] != '\0' &&
            is_full_sha256(entry->sha256)) {
            forge_util_set_error(error, error_size,
                      "%s: dependency '%s' uses the old registry artifact "
                      "lock format; regenerate your lockfile with 'forge update'",
                      path, entry->name);
            (void)fclose(file);
            return -1;
        }
        forge_util_set_error(error, error_size,
                  "%s: dependency '%s' has a malformed lock pin; delete "
                  "Forge.lock and run 'forge update' to regenerate it",
                  path, entry->name);
        (void)fclose(file);
        return -1;
    }
    (void)fclose(file);
    return 0;
}

static int save_lockfile(const char *path, const ForgeLockFile *lock,
                         char *error, size_t error_size)
{
    return forge_util_replace_file(path, write_lockfile_body, (void *)lock,
                                   error, error_size);
}

/* ------------------------------------------------------------------ */
/* Resolution                                                          */
/* ------------------------------------------------------------------ */

typedef struct ForgeResolveContext {
    const char *project_root;
    char lock_path[FORGE_PATH_MAX];
    ForgeLockFile lock;
    int lock_dirty;
    int force_update;
    /* --offline / --locked policies from the CLI (see forge_deps_resolve). */
    int offline;
    int locked;
    /* When non-empty, only this dependency is re-resolved past its lock pin
     * (`forge update <name>`); every other dep keeps its locked commit. */
    char force_update_name[FORGE_MANIFEST_VALUE_MAX];
    ForgeDepGraph *graph;
    ForgeLogger *logger;
    char error[FORGE_COMMAND_MAX];
} ForgeResolveContext;

static ForgeDepNode *graph_find_node(ForgeDepGraph *graph, const char *name)
{
    size_t index;

    if (graph == NULL) {
        return NULL;
    }
    for (index = 0U; index < graph->count; ++index) {
        if (strcmp(graph->nodes[index].name, name) == 0) {
            return &graph->nodes[index];
        }
    }
    return NULL;
}

/* ------------------------------------------------------------------ */
/* Path containment (S3)                                               */
/* ------------------------------------------------------------------ */

/*
 * Compares single path characters loosely: case-insensitively where the host
 * filesystems usually are, and treating '/' and '\\' as interchangeable.
 * Canonical forms produced by forge_paths_absolute use one separator style,
 * but manifests may hand us either spelling, so the comparison stays
 * forgiving rather than rejecting valid directories.
 */
static int loose_path_character_equal(char left, char right)
{
#if FORGE_PLATFORM_WINDOWS
    if (tolower((unsigned char)left) != tolower((unsigned char)right)) {
        int left_is_separator = left == '/' || left == '\\';
        int right_is_separator = right == '/' || right == '\\';

        return left_is_separator && right_is_separator;
    }
    return 1;
#else
    return left == right;
#endif
}

/*
 * True when `candidate` equals `root` or lives underneath it. Both paths must
 * be canonical (no "." or ".." remaining); canonical forms never carry a
 * trailing separator except at a filesystem root ("C:\", "/"), so an exact
 * prefix plus a separator-or-end boundary check is sufficient. On POSIX the
 * canonicalizer resolves symlinks (the directories exist by comparison time),
 * so symlink escapes are caught too; on Windows canonicalization is lexical
 * and a junction/symlink escape is still possible.
 */
static int canonical_path_is_within(const char *candidate, const char *root)
{
    size_t root_length = strlen(root);
    size_t index;
    char boundary;

    for (index = 0U; index < root_length; ++index) {
        if (candidate[index] == '\0' ||
            !loose_path_character_equal(candidate[index], root[index])) {
            return 0;
        }
    }
    if (candidate[root_length] == '\0') {
        return 1;
    }
    boundary = candidate[root_length];
    return boundary == '/' || boundary == '\\';
}

/* Canonicalizes both sides first so callers can pass raw joined paths. */
static int path_is_within(const char *candidate, const char *root)
{
    char canonical_candidate[FORGE_PATH_MAX];
    char canonical_root[FORGE_PATH_MAX];

    if (forge_paths_absolute(candidate, canonical_candidate,
                             sizeof(canonical_candidate)) != 0 ||
        forge_paths_absolute(root, canonical_root, sizeof(canonical_root)) != 0) {
        /* Only buffer overflows fail here; such paths are unusable anyway. */
        return 0;
    }
    return canonical_path_is_within(canonical_candidate, canonical_root);
}

/*
 * S3 gate: true when `consumer_root` sits inside the shared dependency cache.
 * A manifest from the cache is untrusted machine-local input, so its own path
 * dependencies must stay inside its checkout instead of reaching arbitrary
 * directories. The top-level project manifest is not confined — pointing your
 * own project at ../libhello is documented, intended usage.
 */
static int consumer_root_is_cached(const char *consumer_root)
{
    char cache_home[FORGE_PATH_MAX];
    char cache_canonical[FORGE_PATH_MAX];

    if (forge_home_root(cache_home, sizeof(cache_home)) != 0 ||
        forge_paths_absolute(cache_home, cache_canonical,
                             sizeof(cache_canonical)) != 0) {
        return 0;
    }
    return path_is_within(consumer_root, cache_canonical);
}

/* ------------------------------------------------------------------ */
/* Dependency identity (M5)                                            */
/* ------------------------------------------------------------------ */

static int expand_with_stored_defaults(const char *stored,
                                       const ForgeDependency *dependency,
                                       char *out, size_t out_size,
                                       char *error, size_t error_size);

/*
 * Two declarations of the same dependency name may only coexist when they
 * name the same source: identical git URL and ref pair, the same registry
 * package with compatible versions, or the same resolved directory for
 * path deps. Registry compatibility is checked against what actually
 * resolved: exact pins must equal the resolved version, minimums must be
 * satisfied by it, and bare declarations float onto anything. Anything
 * else used to silently resolve to whichever declaration happened to be
 * resolved first. Returns 0 when the sources agree, -1 with a short
 * explanation of the difference in `reason`.
 */
static int dep_identity_conflicts(const ForgeDepNode *node,
                                  const ForgeDependency *dependency,
                                  const char *consumer_root,
                                  char *reason, size_t reason_size)
{
    int node_is_git = !node->is_registry && node->source_url[0] != '\0';
    int dependency_is_git = dependency->git_url[0] != '\0';
    int dependency_is_registry = dependency->registry[0] != '\0';

    if (node->is_registry != dependency_is_registry) {
        forge_util_set_error(reason, reason_size,
                  "one declaration uses %s '%s', the other %s '%s'",
                  node->is_registry ? "registry" :
                  node_is_git ? "git" : "path",
                  node->is_registry ? node->source_url :
                  node_is_git ? node->source_url : node->source_path,
                  dependency_is_registry ? "registry" :
                  dependency_is_git ? "git" : "path",
                  dependency_is_registry ? dependency->registry :
                  dependency_is_git ? dependency->git_url : dependency->path);
        return -1;
    }
    if (node->is_registry) {
        if (strcmp(node->source_url, dependency->registry) != 0) {
            forge_util_set_error(reason, reason_size,
                      "registry packages differ ('%s' vs '%s')",
                      node->source_url, dependency->registry);
            return -1;
        }
        if (dependency->registry_version[0] != '\0') {
            if (strcmp(node->resolved_version,
                       dependency->registry_version) != 0) {
                forge_util_set_error(reason, reason_size,
                          "registry versions differ (resolved '%s' vs "
                          "required '%s')",
                          node->resolved_version[0] != '\0' ?
                              node->resolved_version : "<latest>",
                          dependency->registry_version);
                return -1;
            }
        } else if (dependency->registry_min_version[0] != '\0' &&
            (node->resolved_version[0] == '\0' ||
             forge_version_compare(node->resolved_version,
                                   dependency->registry_min_version) < 0)) {
            forge_util_set_error(reason, reason_size,
                      "registry version '%s' is below the required minimum "
                      "'%s'",
                      node->resolved_version[0] != '\0' ?
                          node->resolved_version : "<latest>",
                      dependency->registry_min_version);
            return -1;
        }
        /*
         * Feature sets compare on effective spelling: the later
         * declaration expands against the recorded defaults. First
         * declaration wins; anything else conflicts loudly rather than
         * rebuilding shared state mid-resolution. This runs for exact
         * pins too: same version with different features is still a
         * divergent diamond.
         */
        {
            char wanted[FORGE_FEATURES_JOINED_MAX];

            if (expand_with_stored_defaults(node->defaults, dependency,
                                            wanted, sizeof(wanted),
                                            reason, reason_size) != 0) {
                return -1;
            }
            if (strcmp(node->features, wanted) != 0) {
                forge_util_set_error(reason, reason_size,
                          "registry features differ (resolved '%s' vs "
                          "required '%s')",
                          node->features[0] != '\0' ? node->features : "<none>",
                          wanted[0] != '\0' ? wanted : "<none>");
                return -1;
            }
        }
        return 0;
    }

    if (node_is_git != dependency_is_git) {
        forge_util_set_error(reason, reason_size,
                  "one declaration uses git '%s', the other path '%s'",
                  node_is_git ? node->source_url : dependency->git_url,
                  dependency_is_git ? dependency->path : node->source_path);
        return -1;
    }
    if (node_is_git) {
        if (strcmp(node->source_url, dependency->git_url) != 0) {
            forge_util_set_error(reason, reason_size,
                      "git URLs differ ('%s' vs '%s')",
                      node->source_url, dependency->git_url);
            return -1;
        }
        if (strcmp(node->source_ref, dependency->ref) != 0) {
            forge_util_set_error(reason, reason_size,
                      "refs differ ('%s' vs '%s')",
                      node->source_ref[0] != '\0' ? node->source_ref : "<default>",
                      dependency->ref[0] != '\0' ? dependency->ref : "<default>");
            return -1;
        }
        return 0;
    }
    {
        char declared[FORGE_PATH_MAX];
        char declared_canonical[FORGE_PATH_MAX];

        /*
         * The same directory spelled relative to two different consumers is
         * one source, not two, so both sides are compared where they land on
         * disk rather than as text. The first occurrence was canonicalized
         * when its node was created. Absolute manifest paths resolve as-is.
         */
        if (forge_paths_resolve(consumer_root, dependency->path, declared,
                                sizeof(declared)) != 0 ||
            forge_paths_absolute(declared, declared_canonical,
                                 sizeof(declared_canonical)) != 0) {
            if (strcmp(node->source_path, declared) != 0) {
                forge_util_set_error(reason, reason_size,
                          "paths differ ('%s' vs '%s')",
                          node->source_path, declared);
                return -1;
            }
            return 0;
        }
        if (!canonical_path_is_within(node->source_path, declared_canonical) &&
            !canonical_path_is_within(declared_canonical, node->source_path)) {
            forge_util_set_error(reason, reason_size,
                      "paths differ ('%s' vs '%s')",
                      node->source_path, declared_canonical);
            return -1;
        }
    }
    return 0;
}

/* ------------------------------------------------------------------ */
/* Registry features                                                   */
/*                                                                     */
/* A declaration selects features; the recipe defines them. Effective  */
/* sets (defaults unless disabled, plus declared) are computed fresh   */
/* from definitions, while later declarations expand against the       */
/* defaults recorded on the existing node (definitions are gone by     */
/* then). First declaration wins: a later set that differs conflicts   */
/* loudly instead of rebuilding shared state mid-resolution.           */
/* ------------------------------------------------------------------ */

/*
 * Folds one resolved feature set into a dependency's own manifest: flag
 * contributions append to both profiles (the sub-build picks by the
 * consumer's options) and feature dependencies queue for the recursion.
 * Feature tables come from the recipe parsed moments ago, so an unknown
 * name here is an internal inconsistency, refused loudly anyway.
 */
static int apply_features(ForgeManifest *parsed, const ForgeFeatureDefs *defs,
                          const char *effective,
                          const char *dep_name, char *error, size_t error_size)
{
    char names[FORGE_DEP_FEATURES_MAX][FORGE_FEATURE_NAME_MAX + 1U];
    size_t count = 0U;
    const char *cursor = effective;
    size_t index;

    /* The effective spelling is canonical output; still bound it. */
    while (*cursor != '\0') {
        const char *comma = strchr(cursor, ',');
        size_t length = comma != NULL ? (size_t)(comma - cursor) : strlen(cursor);

        if (length == 0U || length > FORGE_FEATURE_NAME_MAX ||
            count == FORGE_DEP_FEATURES_MAX) {
            forge_util_set_error(error, error_size,
                      "internal error: feature set for '%s' is malformed",
                      dep_name);
            return -1;
        }
        memcpy(names[count], cursor, length);
        names[count][length] = '\0';
        ++count;
        cursor = comma != NULL ? comma + 1U : cursor + length;
    }
    for (index = 0U; index < count; ++index) {
        const ForgeFeatureDef *def = NULL;
        size_t slot;
        size_t flag;

        for (slot = 0U; slot < defs->count; ++slot) {
            if (strcmp(defs->items[slot].name, names[index]) == 0) {
                def = &defs->items[slot];
                break;
            }
        }
        if (def == NULL) {
            forge_util_set_error(error, error_size,
                      "internal error: feature '%s' for '%s' vanished after "
                      "resolution", names[index], dep_name);
            return -1;
        }
        for (flag = 0U; flag < def->cflag_count; ++flag) {
            if (parsed->debug_profile.cflags.count == FORGE_MANIFEST_MAX_ITEMS ||
                parsed->release_profile.cflags.count == FORGE_MANIFEST_MAX_ITEMS) {
                forge_util_set_error(error, error_size,
                          "dependency '%s': feature '%s' adds more flags "
                          "than fit", dep_name, def->name);
                return -1;
            }
            (void)snprintf(parsed->debug_profile.cflags.items[parsed->debug_profile.cflags.count++],
                           FORGE_MANIFEST_VALUE_MAX, "%s", def->cflags[flag]);
            (void)snprintf(parsed->release_profile.cflags.items[parsed->release_profile.cflags.count++],
                           FORGE_MANIFEST_VALUE_MAX, "%s", def->cflags[flag]);
        }
        for (slot = 0U; slot < def->dep_count; ++slot) {
            const ForgeFeatureDep *want = &def->deps[slot];
            ForgeDependency *dst;

            if (parsed->dependencies.count == FORGE_MANIFEST_MAX_DEPS) {
                forge_util_set_error(error, error_size,
                          "dependency '%s': feature '%s' adds more "
                          "dependencies than fit", dep_name, def->name);
                return -1;
            }
            /* Feature dependencies are registry-only and take the package
             * name as their local name; collisions surface through the
             * usual identity check. They carry no nested feature selection. */
            dst = &parsed->dependencies.items[parsed->dependencies.count++];
            memset(dst, 0, sizeof(*dst));
            (void)snprintf(dst->name, sizeof(dst->name), "%s", want->package);
            (void)snprintf(dst->registry, sizeof(dst->registry), "%s",
                           want->package);
            (void)snprintf(dst->registry_version, sizeof(dst->registry_version),
                           "%s", want->version);
            (void)snprintf(dst->registry_min_version,
                           sizeof(dst->registry_min_version), "%s",
                           want->min_version);
            dst->default_features = 1;
        }
    }
    return 0;
}

/*
 * Expands a later declaration against the defaults recorded on an existing
 * node. Unknown names cannot be told apart from legitimate ones here; they
 * simply never equal the recorded effective set, surfacing as a conflict
 * that names both spellings. Fresh declarations get the precise
 * unknown-feature error instead.
 */
static int expand_with_stored_defaults(const char *stored,
                                       const ForgeDependency *dependency,
                                       char *out, size_t out_size,
                                       char *error, size_t error_size)
{
    char defaults[FORGE_DEP_FEATURES_MAX][FORGE_FEATURE_NAME_MAX + 1U];
    char merged[FORGE_DEP_FEATURES_MAX * 2U][FORGE_FEATURE_NAME_MAX + 1U];
    size_t ndefaults = 0U;
    size_t nmerged = 0U;
    const char *cursor = stored != NULL ? stored : "";
    size_t index;

    for (;;) {
        const char *comma;
        size_t length;

        if (*cursor == '\0') {
            break;
        }
        comma = strchr(cursor, ',');
        length = comma != NULL ? (size_t)(comma - cursor) : strlen(cursor);
        if (length == 0U || length > FORGE_FEATURE_NAME_MAX ||
            ndefaults == FORGE_DEP_FEATURES_MAX) {
            forge_util_set_error(error, error_size,
                      "internal error: recorded feature defaults are malformed");
            return -1;
        }
        memcpy(defaults[ndefaults], cursor, length);
        defaults[ndefaults][length] = '\0';
        ++ndefaults;
        cursor = comma != NULL ? comma + 1U : cursor + length;
    }
    if (dependency->default_features) {
        for (index = 0U; index < ndefaults; ++index) {
            (void)snprintf(merged[nmerged], sizeof(merged[nmerged]), "%s",
                           defaults[index]);
            ++nmerged;
        }
    }
    for (index = 0U; index < dependency->feature_count; ++index) {
        (void)snprintf(merged[nmerged], sizeof(merged[nmerged]), "%s",
                       dependency->features[index]);
        ++nmerged;
    }
    if (forge_features_join(nmerged, merged, out, out_size) != 0) {
        forge_util_set_error(error, error_size,
                  "internal error: feature set is too large");
        return -1;
    }
    return 0;
}

/* ------------------------------------------------------------------ */
/* Cache mutation gate (M6)                                            */
/* ------------------------------------------------------------------ */

/*
 * Two forge processes racing on the same cached repository interleave their
 * clone/fetch/checkout steps and can leave behind half-written state that
 * later runs keep discarding and re-cloning. A create-exclusive sentinel file
 * next to the cache directory serializes them: contenders poll until the
 * holder finishes, and a sentinel orphaned by a killed process is taken over
 * once it is older than the staleness window.
 */
#define FORGE_CACHE_GATE_POLL_MS 100U
#define FORGE_CACHE_GATE_TIMEOUT_SECONDS 30U
#define FORGE_CACHE_GATE_STALE_SECONDS 120U

typedef struct ForgeCacheGate {
    char path[FORGE_PATH_MAX];
    int held;
} ForgeCacheGate;

static void cache_gate_release(ForgeCacheGate *gate)
{
    if (gate->held) {
        (void)remove(gate->path);
        gate->held = 0;
    }
}

/* One creation attempt: 1 when this process owns the sentinel, 0 otherwise. */
static int cache_gate_try_create(const char *path)
{
#if FORGE_PLATFORM_WINDOWS
    HANDLE handle = CreateFileA(path, GENERIC_WRITE, 0U, NULL, CREATE_NEW,
                                FILE_ATTRIBUTE_NORMAL, NULL);

    if (handle == INVALID_HANDLE_VALUE) {
        return 0;
    }
    (void)CloseHandle(handle);
#else
    int descriptor = open(path, O_CREAT | O_EXCL | O_WRONLY, 0644);

    if (descriptor < 0) {
        return 0;
    }
    (void)close(descriptor);
#endif
    return 1;
}

static int cache_gate_acquire(ForgeLogger *logger, const char *cache_dir,
                              ForgeCacheGate *gate, char *error, size_t error_size)
{
    unsigned waited_ms = 0U;

    (void)memset(gate, 0, sizeof(*gate));
    if (snprintf(gate->path, sizeof(gate->path), "%s.lock", cache_dir) < 0 ||
        strlen(cache_dir) + 6U >= sizeof(gate->path)) {
        forge_util_set_error(error, error_size, "dependency cache path is too long");
        return -1;
    }
    for (;;) {
        double age;

        if (cache_gate_try_create(gate->path)) {
            gate->held = 1;
            return 0;
        }
        age = path_age_seconds(gate->path);
        if (age > (double)FORGE_CACHE_GATE_STALE_SECONDS) {
            /*
             * The holder died mid-operation. Stealing is safe because two
             * live processes attempting it still serialize through the
             * exclusive recreate: one wins, so the worst outcome is a
             * redundant re-clone, never interleaved mutations.
             */
            deps_log(logger, "deps",
                     "stale dependency lock '%s' (%.0fs old); taking over",
                     gate->path, age);
            if (remove(gate->path) != 0) {
                deps_sleep_ms(FORGE_CACHE_GATE_POLL_MS);
            }
            continue;
        }
        if (waited_ms >= FORGE_CACHE_GATE_TIMEOUT_SECONDS * 1000U) {
            forge_util_set_error(error, error_size,
                      "another forge process appears to be working in '%s' "
                      "(waited %us); delete '%s' if none is running",
                      cache_dir, FORGE_CACHE_GATE_TIMEOUT_SECONDS, gate->path);
            return -1;
        }
        deps_sleep_ms(FORGE_CACHE_GATE_POLL_MS);
        waited_ms += FORGE_CACHE_GATE_POLL_MS;
    }
}



static int resolve_recursive(ForgeResolveContext *context, const char *consumer_root,
                             const ForgeManifest *manifest, int depth,
                             char names[][FORGE_DEPS_VALUE_MAX])
{
    size_t index;

    if (depth >= (int)FORGE_DEPS_MAX_DEPTH) {
        forge_util_set_error(context->error, sizeof(context->error),
                  "dependency graph is deeper than %d levels", FORGE_DEPS_MAX_DEPTH);
        return -1;
    }
    for (index = 0U; index < manifest->dependencies.count; ++index) {
        const ForgeDependency *dependency = &manifest->dependencies.items[index];
        char root[FORGE_PATH_MAX];
        ForgeDepNode *node;
        ForgeManifest *parsed = NULL;
        int is_native;
        int dep_force;
        size_t stack_index;
        /* Registry resolution outcome, carried to node creation below. */
        char resolved_version[FORGE_MANIFEST_VALUE_MAX];
        unsigned resolved_revision = 0U;
        /* Feature spellings: declared (manifest-canonical) feeds the cache
         * directory; effective/defaults (recipe-resolved) feed the lock,
         * the node, and the sub-build. */
        char declared_joined[FORGE_FEATURES_JOINED_MAX];
        char effective_features[FORGE_FEATURES_JOINED_MAX];
        char default_features[FORGE_FEATURES_JOINED_MAX];
        ForgeFeatureDefs *defs = NULL;

        resolved_version[0] = '\0';
        declared_joined[0] = '\0';
        effective_features[0] = '\0';
        default_features[0] = '\0';
        /* Cycle check along the current path. */
        for (stack_index = 0U; (int)stack_index < depth; ++stack_index) {
            if (strcmp(names[stack_index], dependency->name) == 0) {
                forge_util_set_error(context->error, sizeof(context->error),
                          "dependency cycle detected at '%s'", dependency->name);
                return -1;
            }
        }

        node = graph_find_node(context->graph, dependency->name);
        if (node != NULL) {
            char reason[FORGE_COMMAND_MAX];

            /*
             * A diamond (same name, same source, reached twice) is fine and
             * resolves once. The same name pointing at a different source is
             * a manifest bug: refuse it instead of silently keeping whichever
             * declaration happened to be resolved first.
             */
            if (dep_identity_conflicts(node, dependency, consumer_root, reason,
                                       sizeof(reason)) != 0) {
                forge_util_set_error(context->error, sizeof(context->error),
                          "dependency '%s' is declared twice with conflicting "
                          "sources (%s)", dependency->name, reason);
                return -1;
            }
            continue;
        }
        if (context->graph->count == FORGE_DEPS_MAX_NODES) {
            forge_util_set_error(context->error, sizeof(context->error),
                      "more than %u dependencies", FORGE_DEPS_MAX_NODES);
            return -1;
        }

        /* Global --force wins; otherwise only the dependency named by
         * `forge update <name>` is pulled past its lock pin. */
        dep_force = context->force_update ||
                    (context->force_update_name[0] != '\0' &&
                     strcmp(context->force_update_name, dependency->name) == 0);

        if (dependency->path[0] != '\0') {
            /*
             * forge_paths_resolve, not a blind join: absolute paths in the
             * manifest must land where they say ("C:/..." or "/..."), and a
             * naive concat would bury them inside the consumer's directory.
             */
            if (forge_paths_resolve(consumer_root, dependency->path,
                                    root, sizeof(root)) != 0 ||
                !directory_exists(root)) {
                forge_util_set_error(context->error, sizeof(context->error),
                          "path dependency '%s' does not exist at '%s'",
                          dependency->name, root);
                return -1;
            }
            /*
             * S3 gate: a manifest that lives inside the dependency cache is
             * untrusted input, so its relative paths must stay inside the
             * checkout that shipped them ("../../../elsewhere" stops here).
             * Canonicalization resolves "." and ".." lexically on Windows
             * and, because the directory had to exist to reach this point,
             * symlinks too on POSIX (realpath).
             */
            if (consumer_root_is_cached(consumer_root) &&
                !path_is_within(root, consumer_root)) {
                forge_util_set_error(context->error, sizeof(context->error),
                          "dependency '%s': path '%s' resolves outside its own "
                          "checkout '%s'; cached dependencies may not use "
                          "paths that escape them",
                          dependency->name, dependency->path, consumer_root);
                return -1;
            }
            is_native = dir_has_file(root, "Forge.toml");
        } else if (dependency->registry[0] != '\0') {
            /* Registry source: Sunn yields a source recipe. Materialization
             * reuses the ordinary Git checkout primitive for Git recipes and
             * the ordinary fetch/checksum path for URL recipes. */
            ForgeLockEntry *locked = lock_find(&context->lock, dependency->name);
            char cache_home[FORGE_PATH_MAX];
            char safe_name[FORGE_PATH_MAX];
            char package_dir[FORGE_PATH_MAX];
            ForgeCacheGate gate;
            ForgeRegistryPin pin;
            int reused = 0;
            int fetch_status;

            if (forge_home_root(cache_home, sizeof(cache_home)) != 0) {
                forge_util_set_error(context->error, sizeof(context->error),
                          "cannot resolve '%s': no dependency cache available",
                          dependency->name);
                return -1;
            }
            forge_paths_safe_output_name(dependency->registry, safe_name,
                                         sizeof(safe_name));
            if (snprintf(package_dir, sizeof(package_dir), "%s/registry/%s",
                         cache_home, safe_name) < 0 ||
                strlen(cache_home) + 11U + strlen(safe_name) >= sizeof(package_dir)) {
                forge_util_set_error(context->error, sizeof(context->error),
                          "dependency cache path is too long");
                return -1;
            }
            if (forge_paths_ensure_directory(package_dir, context->error,
                                              sizeof(context->error)) != 0) {
                forge_util_prepend_error(context->error, sizeof(context->error),
                          "cannot resolve '%s': ", dependency->name);
                return -1;
            }
            /* M6 gate, same as git checkouts: one writer per package dir. */
            if (cache_gate_acquire(context->logger, package_dir, &gate,
                                   context->error,
                                   sizeof(context->error)) != 0) {
                return -1;
            }
            if (forge_features_join(dependency->feature_count,
                                    dependency->features,
                                    declared_joined,
                                    sizeof(declared_joined)) != 0) {
                cache_gate_release(&gate);
                forge_util_set_error(context->error, sizeof(context->error),
                          "dependency '%s': feature set is too large",
                          dependency->name);
                return -1;
            }
            defs = calloc(1U, sizeof(*defs));
            if (defs == NULL) {
                cache_gate_release(&gate);
                forge_util_set_error(context->error, sizeof(context->error),
                          "out of memory while resolving dependencies");
                return -1;
            }
            fetch_status = forge_registry_materialize(
                context->logger, dependency->name, dependency->registry,
                dependency->registry_version,
                dependency->registry_min_version,
                declared_joined, dependency->default_features,
                locked != NULL ? locked->version : "",
                locked != NULL ? locked->kind : "",
                locked != NULL ? locked->location : "",
                locked != NULL ? locked->ref : "",
                locked != NULL ? locked->commit : "",
                locked != NULL ? locked->sha256 : "",
                locked != NULL ? locked->revision : 0U,
                locked != NULL ? locked->features : "",
                dep_force, context->offline, package_dir,
                root, sizeof(root), &pin, defs, &reused,
                context->error, sizeof(context->error));
            cache_gate_release(&gate);
            if (fetch_status != 0) {
                free(defs);
                return -1;
            }
            deps_log(context->logger, "deps", "resolved %s %s%s",
                     dependency->name, pin.version,
                     reused ? " (cached)" : "");

            if (pin.kind[0] == '\0' || pin.location[0] == '\0') {
                forge_util_set_error(context->error, sizeof(context->error),
                          "internal error: registry pin for '%s' has no source",
                          dependency->name);
                free(defs);
                return -1;
            }
            (void)snprintf(resolved_version, sizeof(resolved_version), "%s",
                           pin.version);
            resolved_revision = pin.revision;
            /*
             * Effective features: recipe defaults (unless disabled) plus
             * the declared request, validated against the recipe just
             * fetched. Unknown names fail here naming what exists.
             *
             * A cache hit without reachable definitions (remote registry
             * while --offline) cannot revalidate: trust the lock's
             * recorded set, which was validated when first resolved.
             */
            if (reused && defs->default_count == 0U && defs->count == 0U &&
                (dependency->feature_count != 0U ||
                 (locked != NULL && locked->features[0] != '\0'))) {
                if (locked == NULL ||
                    snprintf(effective_features, sizeof(effective_features),
                             "%s", locked->features) < 0 ||
                    strlen(locked->features) >= sizeof(effective_features)) {
                    free(defs);
                    forge_util_set_error(context->error, sizeof(context->error),
                              "dependency '%s': feature set is too large",
                              dependency->name);
                    return -1;
                }
                default_features[0] = '\0';
            } else if (forge_features_join(defs->default_count, defs->defaults,
                                     default_features,
                                     sizeof(default_features)) != 0 ||
                 forge_features_effective(defs, dependency->features,
                                          dependency->feature_count,
                                          dependency->default_features,
                                          dependency->name,
                                          effective_features,
                                          sizeof(effective_features),
                                          context->error,
                                          sizeof(context->error)) != 0) {
                free(defs);
                return -1;
            }

            /* Record/update the pin; a kind change from git silently
             * converts, exactly like a changed git URL does — the
             * manifest is the source of truth and --locked catches
             * anything unintended in CI. */
            if (locked == NULL) {
                if (context->lock.count == FORGE_LOCK_MAX_ENTRIES) {
                    forge_util_set_error(context->error, sizeof(context->error),
                              "more than %u locked dependencies", FORGE_LOCK_MAX_ENTRIES);
                    free(defs);
                    return -1;
                }
                locked = &context->lock.items[context->lock.count++];
                (void)snprintf(locked->name, sizeof(locked->name), "%s", dependency->name);
            }
            if (strcmp(locked->kind, pin.kind) != 0 ||
                strcmp(locked->version, pin.version) != 0 ||
                strcmp(locked->location, pin.location) != 0 ||
                strcmp(locked->ref, pin.ref) != 0 ||
                strcmp(locked->commit, pin.commit) != 0 ||
                strcmp(locked->sha256, pin.sha256) != 0 ||
                locked->revision != pin.revision ||
                strcmp(locked->features, effective_features) != 0) {
                (void)snprintf(locked->kind, sizeof(locked->kind), "%s", pin.kind);
                (void)snprintf(locked->version, sizeof(locked->version), "%s", pin.version);
                (void)snprintf(locked->location, sizeof(locked->location), "%s", pin.location);
                (void)snprintf(locked->ref, sizeof(locked->ref), "%s", pin.ref);
                (void)snprintf(locked->commit, sizeof(locked->commit), "%s", pin.commit);
                (void)snprintf(locked->sha256, sizeof(locked->sha256), "%s", pin.sha256);
                locked->revision = pin.revision;
                (void)snprintf(locked->features, sizeof(locked->features), "%s",
                               effective_features);
                context->lock_dirty = 1;
            }
            is_native = dir_has_file(root, "Forge.toml");
            /*
             * Feature flags only reach native sub-builds through Forge.toml
             * profiles; on a foreign (CMake/Make) dependency they would be
             * silently ignored, so refuse loudly instead.
             */
            if (!is_native && effective_features[0] != '\0') {
                forge_util_set_error(context->error, sizeof(context->error),
                          "dependency '%s': features ('%s') need a native "
                          "Forge.toml dependency",
                          dependency->name, effective_features);
                free(defs);
                return -1;
            }
            if (!is_native) {
                free(defs);
                defs = NULL;
            }
        } else {
            ForgeLockEntry *locked = lock_find(&context->lock, dependency->name);
            char cache_home[FORGE_PATH_MAX];
            char cache_hash[32];
            char cache_dir[FORGE_PATH_MAX];
            char resolved_sha[FORGE_DEPS_VALUE_MAX];
            char url_reason[FORGE_COMMAND_MAX];

            /* K1 gate: the URL becomes a lone clone argument, so anything
             * outside the allowlist must fail here, before git runs. */
            if (forge_deps_git_url_is_supported(dependency->git_url, url_reason,
                                                sizeof(url_reason)) != 0) {
                forge_util_set_error(context->error, sizeof(context->error),
                          "dependency '%s': %s", dependency->name, url_reason);
                return -1;
            }
            if (git_available(context->error, sizeof(context->error)) != 0 ||
                forge_home_root(cache_home, sizeof(cache_home)) != 0) {
                forge_util_set_error(context->error, sizeof(context->error),
                          "cannot resolve '%s': no dependency cache available",
                          dependency->name);
                return -1;
            }
            url_cache_hash(dependency->git_url, cache_hash, sizeof(cache_hash));
            if (snprintf(cache_dir, sizeof(cache_dir), "%s/git/%s", cache_home,
                         cache_hash) < 0 ||
                strlen(cache_home) + 22U >= sizeof(cache_dir)) {
                forge_util_set_error(context->error, sizeof(context->error),
                          "dependency cache path is too long");
                return -1;
            }
            /*
             * The mutation gate below drops its sentinel next to the cache
             * directory, so the shared cache root must exist even before the
             * very first clone.
             */
            {
                char cache_git_root[FORGE_PATH_MAX];

                if (forge_paths_join(cache_git_root, sizeof(cache_git_root),
                                     cache_home, "git") != 0 ||
                    forge_paths_ensure_directory(cache_git_root, context->error,
                                                 sizeof(context->error)) != 0) {
                    forge_util_prepend_error(context->error, sizeof(context->error),
                              "cannot resolve '%s': ", dependency->name);
                    return -1;
                }
            }
            /*
             * M6 gate: serialize clone/fetch/checkout against other forge
             * processes targeting the same cached repository.
             */
            {
                ForgeCacheGate gate;
                int checkout_status;

                if (cache_gate_acquire(context->logger, cache_dir, &gate,
                                       context->error,
                                       sizeof(context->error)) != 0) {
                    return -1;
                }
                checkout_status = forge_deps_ensure_git_checkout(
                    context->logger, dependency->name,
                    dependency->git_url, dependency->ref,
                    locked != NULL &&
                    strcmp(locked->url, dependency->git_url) == 0 &&
                    strcmp(locked->ref, dependency->ref) == 0 ?
                        locked->commit : "",
                    dep_force, dependency->submodules, context->offline,
                    cache_dir,
                    resolved_sha, sizeof(resolved_sha),
                    context->error, sizeof(context->error));
                cache_gate_release(&gate);
                if (checkout_status != 0) {
                    return -1;
                }
            }
            deps_log(context->logger, "deps", "resolved %s @ %s",
                     dependency->name, resolved_sha);

            /* Record/update the pin. */
            if (locked == NULL) {
                if (context->lock.count == FORGE_LOCK_MAX_ENTRIES) {
                    forge_util_set_error(context->error, sizeof(context->error),
                              "more than %u locked dependencies", FORGE_LOCK_MAX_ENTRIES);
                    return -1;
                }
                locked = &context->lock.items[context->lock.count++];
                (void)snprintf(locked->name, sizeof(locked->name), "%s", dependency->name);
            }
            if (strcmp(locked->commit, resolved_sha) != 0 ||
                strcmp(locked->url, dependency->git_url) != 0 ||
                strcmp(locked->ref, dependency->ref) != 0) {
                (void)snprintf(locked->commit, sizeof(locked->commit), "%s", resolved_sha);
                (void)snprintf(locked->url, sizeof(locked->url), "%s", dependency->git_url);
                (void)snprintf(locked->ref, sizeof(locked->ref), "%s", dependency->ref);
                locked->version[0] = '\0';
                locked->sha256[0] = '\0';
                context->lock_dirty = 1;
            }
            (void)snprintf(root, sizeof(root), "%s", cache_dir);
            is_native = dir_has_file(root, "Forge.toml");
        }

        /* Parse native manifests and recurse before inserting this node so
         * dependencies always precede their consumers in build order. */
        if (is_native) {
            char manifest_path[FORGE_PATH_MAX];

            parsed = calloc(1U, sizeof(*parsed));
            if (parsed == NULL) {
                forge_util_set_error(context->error, sizeof(context->error),
                          "out of memory while resolving dependencies");
                return -1;
            }
            if (forge_paths_join(manifest_path, sizeof(manifest_path), root,
                                 "Forge.toml") != 0 ||
                forge_manifest_load(manifest_path, parsed, context->error,
                                    sizeof(context->error)) != 0) {
                forge_util_prepend_error(context->error, sizeof(context->error),
                          "dependency '%s': ", dependency->name);
                free(parsed);
                free(defs);
                return -1;
            }
            /*
             * Feature selection: fold each effective feature's flags into
             * both profiles (the sub-build picks by the consumer's
             * options) and queue its extra dependencies for the recursion
             * below. Definitions came from the recipe just fetched.
             * Skipped when a cache hit could not reach definitions
             * (remote registry while --offline): the set still comes
             * from the lock, and cached objects keep their flags.
             */
            if (effective_features[0] != '\0' && defs->count != 0U &&
                apply_features(parsed, defs, effective_features,
                               dependency->name,
                               context->error, sizeof(context->error)) != 0) {
                free(parsed);
                free(defs);
                return -1;
            }
            free(defs);
            defs = NULL;
            names[depth][0] = '\0';
            (void)snprintf(names[depth], FORGE_DEPS_VALUE_MAX, "%s", dependency->name);
            if (resolve_recursive(context, root, parsed, depth + 1, names) != 0) {
                free(parsed);
                return -1;
            }
        }

        /*
         * The recursion above can fill any remaining slots, so capacity must
         * be re-checked at the point of insertion — checking only before the
         * recursive descent let a deep subtree write past nodes[].
         */
        if (context->graph->count >= FORGE_DEPS_MAX_NODES) {
            free(parsed);
            forge_util_set_error(context->error, sizeof(context->error),
                      "more than %u dependencies", FORGE_DEPS_MAX_NODES);
            return -1;
        }
        node = &context->graph->nodes[context->graph->count++];
        (void)snprintf(node->name, sizeof(node->name), "%s", dependency->name);
        (void)snprintf(node->root, sizeof(node->root), "%s", root);
        node->source_url[0] = '\0';
        node->source_ref[0] = '\0';
        node->source_path[0] = '\0';
        node->is_registry = 0;
        node->resolved_version[0] = '\0';
        node->resolved_revision = 0U;
        if (dependency->registry[0] != '\0') {
            (void)snprintf(node->source_url, sizeof(node->source_url), "%s",
                           dependency->registry);
            (void)snprintf(node->source_ref, sizeof(node->source_ref), "%s",
                           dependency->registry_version);
            node->is_registry = 1;
            (void)snprintf(node->resolved_version, sizeof(node->resolved_version),
                           "%s", resolved_version);
            node->resolved_revision = resolved_revision;
            (void)snprintf(node->features, sizeof(node->features), "%s",
                           effective_features);
            (void)snprintf(node->defaults, sizeof(node->defaults), "%s",
                           default_features);
        } else if (dependency->git_url[0] != '\0') {
            (void)snprintf(node->source_url, sizeof(node->source_url), "%s",
                           dependency->git_url);
            (void)snprintf(node->source_ref, sizeof(node->source_ref), "%s",
                           dependency->ref);
        } else {
            /*
             * Canonical spelling so the same directory reached through two
             * different consumers compares equal (M5).
             */
            if (forge_paths_absolute(root, node->source_path,
                                     sizeof(node->source_path)) != 0) {
                (void)snprintf(node->source_path, sizeof(node->source_path),
                               "%s", root);
            }
        }
        node->manifest = parsed;
        node->is_native = is_native;
        node->link_artifact[0] = '\0';
        deps_log(context->logger, "deps", "using %s (%s)", dependency->name, root);
    }
    return 0;
}

/*
 * Drops lock entries for dependencies that are no longer reachable from the
 * manifest (removed directly, or via a transitive consumer), so Forge.lock
 * always mirrors the graph that was just resolved.
 */
static void prune_lock_entries(ForgeLockFile *lock, const ForgeDepGraph *graph,
                               int *dirty)
{
    size_t index;
    size_t kept = 0U;

    for (index = 0U; index < lock->count; ++index) {
        if (graph_find_node((ForgeDepGraph *)graph, lock->items[index].name) == NULL) {
            *dirty = 1;
            continue;
        }
        if (kept != index) {
            lock->items[kept] = lock->items[index];
        }
        ++kept;
    }
    lock->count = kept;
}

int forge_deps_resolve(const char *project_root, const ForgeManifest *manifest,
                       int force_update, const char *force_update_name,
                       int offline, int locked,
                       ForgeDepGraph *graph, ForgeLogger *logger,
                       char *error, size_t error_size)
{
    ForgeResolveContext context;
    char names[FORGE_DEPS_MAX_DEPTH][FORGE_MANIFEST_VALUE_MAX];
    int status;
    int dirty;

    if (project_root == NULL || manifest == NULL || graph == NULL) {
        forge_util_set_error(error, error_size, "project root, manifest, and graph are required");
        return -1;
    }
    memset(graph, 0, sizeof(*graph));
    memset(&context, 0, sizeof(context));
    context.project_root = project_root;
    context.force_update = force_update;
    context.offline = offline;
    context.locked = locked;
    if (force_update_name != NULL) {
        (void)snprintf(context.force_update_name,
                       sizeof(context.force_update_name), "%s", force_update_name);
    }
    context.graph = graph;
    context.logger = logger;
    if (forge_paths_join(context.lock_path, sizeof(context.lock_path), project_root,
                         "Forge.lock") != 0) {
        forge_util_set_error(error, error_size, "lockfile path is too long");
        return -1;
    }
    if (load_lockfile(context.lock_path, &context.lock, error, error_size) != 0) {
        return -1;
    }
    /*
     * No [dependencies] at all is still meaningful: every previously locked
     * dependency is now unreachable and must leave the lockfile.
     */
    if (manifest->dependencies.count == 0U) {
        if (context.lock.count != 0U) {
            if (locked) {
                forge_util_set_error(error, error_size,
                          "--locked forbids changing Forge.lock but it lists "
                          "dependencies that are no longer declared; rerun "
                          "without --locked to prune them");
                return -1;
            }
            context.lock.count = 0U;
            if (save_lockfile(context.lock_path, &context.lock, error, error_size) != 0) {
                return -1;
            }
        }
        return 0;
    }
    status = resolve_recursive(&context, project_root, manifest, 0, names);
    if (status == 0) {
        prune_lock_entries(&context.lock, graph, &dirty);
    } else {
        dirty = 0;
    }
    if (status != 0) {
        (void)snprintf(error, error_size, "%s", context.error);
        return status;
    }
    if (locked && (context.lock_dirty || dirty)) {
        /* --locked promises the resolution leaves every pin exactly as
         * committed; a moved/added/pruned pin means the caller is out of
         * sync with the manifest. */
        forge_util_set_error(error, error_size,
                  "--locked forbids changing Forge.lock but this resolution "
                  "needs to update a pin; rerun without --locked (or `forge "
                  "update`) and commit the result");
        return -1;
    }
    if (context.lock_dirty || dirty || !context.lock.exists) {
        if (save_lockfile(context.lock_path, &context.lock, error, error_size) != 0) {
            return -1;
        }
    }
    return 0;
}

void forge_deps_free_graph(ForgeDepGraph *graph)
{
    size_t index;

    if (graph == NULL) {
        return;
    }
    for (index = 0U; index < graph->count; ++index) {
        free(graph->nodes[index].manifest);
        graph->nodes[index].manifest = NULL;
    }
}

int forge_deps_include_dirs(const ForgeDepGraph *graph, ForgeStringList *output,
                            char *error, size_t error_size)
{
    size_t index;

    for (index = 0U; index < graph->count; ++index) {
        const ForgeDepNode *node = &graph->nodes[index];
        char candidate[FORGE_PATH_MAX];

        if (output->count == FORGE_MANIFEST_MAX_ITEMS) {
            forge_util_set_error(error, error_size, "too many include directories");
            return -1;
        }
        if (forge_paths_join(candidate, sizeof(candidate), node->root, "include") != 0) {
            forge_util_set_error(error, error_size, "include path is too long");
            return -1;
        }
        {
            const char *chosen = directory_exists(candidate) ? candidate : node->root;

            if (strlen(chosen) >= sizeof(output->items[output->count])) {
                forge_util_set_error(error, error_size,
                          "include path '%s' is too long", chosen);
                return -1;
            }
            memcpy(output->items[output->count], chosen, strlen(chosen) + 1U);
        }
        ++output->count;
    }
    return 0;
}

/* ------------------------------------------------------------------ */
/* Foreign builds                                                      */
/* ------------------------------------------------------------------ */

static int has_static_lib_suffix(const char *name)
{
    return forge_util_has_suffix(name, ".a") || forge_util_has_suffix(name, ".lib");
}

/*
 * Depth-first search of `dir` for the first static library. Returns 1 when
 * one was copied into `found`, 0 when none was found, -1 on a hard error.
 * Generated/build directories that never hold the final artifact are pruned
 * to keep the scan bounded by `budget` entries. Symlinks are never followed:
 * a dependency containing `build -> /usr/lib` must not resolve its artifact
 * outside its own root (nor loop forever), so linked entries are skipped and
 * nesting past FORGE_SCAN_MAX_DEPTH is a hard error.
 */
static int scan_dir_for_static_lib(const char *dir, char *found, size_t found_size,
                                    unsigned *budget, unsigned depth)
{
#if FORGE_PLATFORM_WINDOWS
    WIN32_FIND_DATAA entry;
    HANDLE handle;
    char pattern[FORGE_PATH_MAX];
#else
    DIR *stream;
    struct dirent *item;
#endif

    if (*budget == 0U) {
        return 0;
    }
    if (depth >= FORGE_SCAN_MAX_DEPTH) {
        return -1;
    }
    --*budget;
#if FORGE_PLATFORM_WINDOWS
    if (forge_paths_join(pattern, sizeof(pattern), dir, "*") != 0) {
        return -1;
    }
    handle = FindFirstFileA(pattern, &entry);
    if (handle == INVALID_HANDLE_VALUE) {
        return 0;
    }
    do {
        char child[FORGE_PATH_MAX];
        int status;

        if (strcmp(entry.cFileName, ".") == 0 || strcmp(entry.cFileName, "..") == 0) {
            continue;
        }
        if (forge_paths_join(child, sizeof(child), dir, entry.cFileName) != 0) {
            (void)FindClose(handle);
            return -1;
        }
        if ((entry.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) != 0U) {
            continue;
        }
        if ((entry.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0U) {
            if (strcmp(entry.cFileName, "CMakeFiles") == 0 ||
                strcmp(entry.cFileName, ".git") == 0 ||
                strcmp(entry.cFileName, "target") == 0) {
                continue;
            }
            status = scan_dir_for_static_lib(child, found, found_size, budget,
                                             depth + 1U);
            if (status != 0) {
                (void)FindClose(handle);
                return status;
            }
        } else if (has_static_lib_suffix(entry.cFileName)) {
            if (snprintf(found, found_size, "%s", child) < 0 ||
                strlen(child) >= found_size) {
                (void)FindClose(handle);
                return -1;
            }
            (void)FindClose(handle);
            return 1;
        }
    } while (FindNextFileA(handle, &entry) != 0);
    if (GetLastError() != ERROR_NO_MORE_FILES) {
        (void)FindClose(handle);
        return -1;
    }
    (void)FindClose(handle);
    return 0;
#else
    stream = opendir(dir);
    if (stream == NULL) {
        return errno == ENOENT ? 0 : -1;
    }
    while ((item = readdir(stream)) != NULL) {
        char child[FORGE_PATH_MAX];
        struct stat details;
        int status;

        if (strcmp(item->d_name, ".") == 0 || strcmp(item->d_name, "..") == 0) {
            continue;
        }
        if (forge_paths_join(child, sizeof(child), dir, item->d_name) != 0) {
            (void)closedir(stream);
            return -1;
        }
        if (lstat(child, &details) != 0) {
            continue;
        }
        if (S_ISLNK(details.st_mode)) {
            continue;
        }
        if (S_ISDIR(details.st_mode)) {
            if (strcmp(item->d_name, "CMakeFiles") == 0 ||
                strcmp(item->d_name, ".git") == 0 ||
                strcmp(item->d_name, "target") == 0) {
                continue;
            }
            status = scan_dir_for_static_lib(child, found, found_size, budget,
                                             depth + 1U);
            if (status != 0) {
                (void)closedir(stream);
                return status;
            }
        } else if (has_static_lib_suffix(item->d_name)) {
            if (snprintf(found, found_size, "%s", child) < 0 ||
                strlen(child) >= found_size) {
                (void)closedir(stream);
                return -1;
            }
            (void)closedir(stream);
            return 1;
        }
    }
    (void)closedir(stream);
    return 0;
#endif
}

static int find_static_artifact(const char *root, char *artifact, size_t artifact_size,
                                char *error, size_t error_size)
{
    static const char *const search_dirs[] = { "build", "lib", "." };
    size_t index;

    for (index = 0U; index < 3U; ++index) {
        char candidate[FORGE_PATH_MAX];
        unsigned budget = FORGE_SCAN_ENTRY_LIMIT;
        int status;

        if (forge_paths_join(candidate, sizeof(candidate), root,
                             search_dirs[index]) != 0) {
            continue;
        }
        status = scan_dir_for_static_lib(candidate, artifact, artifact_size, &budget,
                                             0U);
        if (status < 0) {
            forge_util_set_error(error, error_size,
                      "could not search '%s' for a built library", candidate);
            return -1;
        }
        if (status > 0) {
            return 0;
        }
    }
    return 1; /* nothing found */
}

/* ------------------------------------------------------------------ */
/* Build-script trust (S1)                                             */
/* ------------------------------------------------------------------ */

#define FORGE_SCRIPTS_MARKER ".forge-scripts-approved"

/* True only for a genuine regular file: symlinks (and Windows reparse
 * points) never count, so a dependency cannot smuggle approval past the
 * prompt with a marker link, nor redirect the approval write at a victim. */
static int marker_is_approved(const char *path)
{
#if FORGE_PLATFORM_WINDOWS
    DWORD attributes = GetFileAttributesA(path);

    return attributes != INVALID_FILE_ATTRIBUTES &&
           (attributes & FILE_ATTRIBUTE_DIRECTORY) == 0U &&
           (attributes & FILE_ATTRIBUTE_REPARSE_POINT) == 0U;
#else
    struct stat details;

    return lstat(path, &details) == 0 && S_ISREG(details.st_mode);
#endif
}

/* Records approval atomically: O_CREAT|O_EXCL (plus O_NOFOLLOW where the
 * platform has it) so a concurrently planted link can neither divert the
 * write nor slip past the check above. Returns 0 when the marker exists
 * afterwards as a genuine file, -1 otherwise. */
static int marker_record_approval(const char *path)
{
#if FORGE_PLATFORM_WINDOWS
    int fd = _open(path, _O_WRONLY | _O_CREAT | _O_EXCL,
                   _S_IREAD | _S_IWRITE);
#else
    int flags = O_WRONLY | O_CREAT | O_EXCL;
#ifdef O_NOFOLLOW
    flags |= O_NOFOLLOW;
#endif
    int fd = open(path, flags, 0600);
#endif
    FILE *file;

    if (fd < 0) {
        return marker_is_approved(path) ? 0 : -1;
    }
#if FORGE_PLATFORM_WINDOWS
    file = _fdopen(fd, "w");
#else
    file = fdopen(fd, "w");
#endif
    if (file == NULL) {
#if FORGE_PLATFORM_WINDOWS
        _close(fd);
#else
        (void)close(fd);
#endif
        (void)remove(path);
        return -1;
    }
    (void)fputs("approved\n", file);
    if (fclose(file) != 0) {
        (void)remove(path);
        return -1;
    }
    return 0;
}

/*
 * Building a foreign dependency runs its cmake/make scripts — third-party
 * code execution on every build. Gate the first run behind an explicit
 * decision:
 *
 *   FORGE_ALLOW_DEP_BUILD_SCRIPTS=0   refuse outright (CI hardening)
 *   FORGE_ALLOW_DEP_BUILD_SCRIPTS=1   pre-approve everything
 *   otherwise                          ask once per dependency checkout;
 *                                      approval is recorded next to the
 *                                      clone so a fresh clone re-asks.
 */
static int build_scripts_allowed(ForgeLogger *logger, const char *dependency_name,
                                  const char *dependency_root, const char *kind,
                                  char *error, size_t error_size)
{
    const char *override = getenv("FORGE_ALLOW_DEP_BUILD_SCRIPTS");
    char marker_path[FORGE_PATH_MAX];

    if (override != NULL && override[0] != '\0') {
        if (strcmp(override, "0") == 0) {
            forge_util_set_error(error, error_size,
                      "dependency '%s' needs to run %s build scripts, but "
                      "FORGE_ALLOW_DEP_BUILD_SCRIPTS=0 forbids them",
                      dependency_name, kind);
            return -1;
        }
        return 0;
    }
    if (forge_paths_join(marker_path, sizeof(marker_path), dependency_root,
                         FORGE_SCRIPTS_MARKER) != 0) {
        forge_util_set_error(error, error_size, "dependency path is too long");
        return -1;
    }
    if (marker_is_approved(marker_path)) {
        return 0;
    }
#if FORGE_PLATFORM_WINDOWS
    if (_isatty(_fileno(stdin)) == 0) {
#else
    if (isatty(fileno(stdin)) == 0) {
#endif
        forge_util_set_error(error, error_size,
                  "dependency '%s' wants to run %s build scripts, but no "
                  "terminal is attached to approve them; set "
                  "FORGE_ALLOW_DEP_BUILD_SCRIPTS=1 to allow (or =0 to deny)",
                  dependency_name, kind);
        return -1;
    }
    fprintf(stderr,
            "forge: dependency '%s' (%s)\n"
            "forge: will now execute third-party %s build scripts.\n"
            "forge: Allow? [y/N] ",
            dependency_name, dependency_root, kind);
    (void)fflush(stderr);
    {
        int answer = fgetc(stdin);

        if (answer != 'y' && answer != 'Y') {
            forge_util_set_error(error, error_size,
                      "dependency '%s' was not approved to run %s build "
                      "scripts; set FORGE_ALLOW_DEP_BUILD_SCRIPTS=0 to make "
                      "this refusal permanent",
                      dependency_name, kind);
            return -1;
        }
    }
    /* Record the decision so later builds of this checkout stay quiet. A
     * pre-existing genuine marker (won race) is approval too; anything else
     * keeps the prompt for next time. */
    if (marker_record_approval(marker_path) != 0) {
        deps_log(logger, "deps",
                 "could not record script approval for '%s'; it will be "
                 "requested again", dependency_name);
    }
    return 0;
}

int forge_deps_build_foreign(const ForgeDepNode *node, const ForgeCompiler *compiler,
                             int release, int max_jobs, ForgeLogger *logger,
                             char *artifact, size_t artifact_size,
                             char *error, size_t error_size)
{
    const char *build_type = release ? "Release" : "Debug";
    const char *capture_path = logger != NULL ? logger->path : NULL;
    char jobs[16];
    const char *kind = NULL;
    int exit_code = 0;
    int status;

    if (dir_has_file(node->root, "CMakeLists.txt")) {
        kind = "CMake";
    } else if (dir_has_file(node->root, "Makefile") ||
               dir_has_file(node->root, "makefile") ||
               dir_has_file(node->root, "GNUmakefile")) {
        kind = "Make";
    }
    if (kind != NULL &&
        build_scripts_allowed(logger, node->name, node->root, kind, error,
                              error_size) != 0) {
        return -1;
    }
    (void)snprintf(jobs, sizeof(jobs), "%d",
                   max_jobs > 0 ? max_jobs : forge_thread_processor_count());
    deps_log(logger, "deps", "building foreign dependency '%s' in %s",
             node->name, node->root);
    if (dir_has_file(node->root, "CMakeLists.txt")) {
        ForgeArgv argv = {0};

        if (!forge_util_program_available("cmake")) {
            forge_util_set_error(error, error_size,
                      "dependency '%s' uses CMake but cmake was not found on PATH",
                      node->name);
            return -1;
        }
        if (forge_argv_append(&argv, "cmake") != 0 ||
            forge_argv_append(&argv, "-S") != 0 ||
            forge_argv_append(&argv, node->root) != 0 ||
            forge_argv_append(&argv, "-B") != 0 ||
            forge_argv_appendf(&argv, "%s/build", node->root) != 0 ||
            forge_argv_appendf(&argv, "-DCMAKE_BUILD_TYPE=%s", build_type) != 0 ||
            run_tool(logger, "deps", &argv, capture_path, 1, &exit_code,
                     error, error_size) != 0 ||
            exit_code != 0) {
            forge_util_set_error(error, error_size,
                      "cmake configure failed for dependency '%s'", node->name);
            forge_argv_free(&argv);
            return -1;
        }
        forge_argv_free(&argv);
        if (forge_argv_append(&argv, "cmake") != 0 ||
            forge_argv_append(&argv, "--build") != 0 ||
            forge_argv_appendf(&argv, "%s/build", node->root) != 0 ||
            forge_argv_append(&argv, "--config") != 0 ||
            forge_argv_append(&argv, build_type) != 0 ||
            forge_argv_append(&argv, "--parallel") != 0 ||
            forge_argv_append(&argv, jobs) != 0 ||
            run_tool(logger, "deps", &argv, capture_path, 1, &exit_code,
                     error, error_size) != 0 ||
            exit_code != 0) {
            forge_util_set_error(error, error_size,
                      "cmake build failed for dependency '%s'", node->name);
            forge_argv_free(&argv);
            return -1;
        }
        forge_argv_free(&argv);
    } else if (dir_has_file(node->root, "Makefile") ||
               dir_has_file(node->root, "makefile") ||
               dir_has_file(node->root, "GNUmakefile")) {
        ForgeArgv argv = {0};

        if (!forge_util_program_available("make")) {
            forge_util_set_error(error, error_size,
                      "dependency '%s' uses Make but make was not found on PATH",
                      node->name);
            return -1;
        }
        if (forge_argv_append(&argv, "make") != 0 ||
            forge_argv_append(&argv, "-C") != 0 ||
            forge_argv_append(&argv, node->root) != 0 ||
            forge_argv_append(&argv, "-j") != 0 ||
            forge_argv_append(&argv, jobs) != 0) {
            forge_util_set_error(error, error_size,
                      "out of memory while building a make command");
            forge_argv_free(&argv);
            return -1;
        }
        if (compiler != NULL && compiler->kind != FORGE_COMPILER_MSVC &&
            forge_argv_appendf(&argv, "CC=%s", compiler->program) != 0) {
            forge_util_set_error(error, error_size,
                      "out of memory while building a make command");
            forge_argv_free(&argv);
            return -1;
        }
        if (run_tool(logger, "deps", &argv, capture_path, 1, &exit_code,
                     error, error_size) != 0 ||
            exit_code != 0) {
            forge_util_set_error(error, error_size,
                      "make failed for dependency '%s'", node->name);
            forge_argv_free(&argv);
            return -1;
        }
        forge_argv_free(&argv);
    } else {
        forge_util_set_error(error, error_size,
                  "dependency '%s' has no Forge.toml, CMakeLists.txt, or Makefile; "
                  "forge does not know how to build it", node->name);
        return -1;
    }

    status = find_static_artifact(node->root, artifact, artifact_size, error, error_size);
    if (status != 0) {
        if (status > 0) {
            forge_util_set_error(error, error_size,
                      "built '%s' but no static library (.a/.lib) was found",
                      node->name);
        }
        return -1;
    }
    deps_log(logger, "deps", "artifact: %s", artifact);
    return 0;
}
