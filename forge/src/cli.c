#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "forge/cli.h"
#include "forge/orchestrator.h"
#include "forge/paths.h"
#include "forge/version.h"

#define FORGE_MAX_PROGRAM_ARGUMENTS 256U

static void print_usage(FILE *stream)
{
    fprintf(stream,
            "Forge — C/C++/assembly build orchestrator (v" FORGE_VERSION ")\n"
            "Usage:\n"
            "  forge build [--release] [--jobs N] [--manifest PATH]\n"
            "  forge check [--release] [--jobs N] [--manifest PATH]\n"
            "  forge run [--release] [--jobs N] [--manifest PATH] [-- ARGS...]\n"
            "  forge test [--release] [--jobs N] [--test NAME] [--manifest PATH]\n"
            "  forge debug [--release] [--jobs N] [--manifest PATH]\n"
            "  forge clean [--manifest PATH]\n"
            "  forge update [NAME] [--offline] [--manifest PATH]\n"
            "                        re-resolve deps (one, or all when NAME is omitted)\n"
            "  forge add <NAME> --git URL [--tag T | --branch B | --rev R]\n"
            "  forge add <NAME> --path DIR      [--manifest PATH]\n"
            "  forge add <NAME> --registry PKG [--version VER]\n"
            "      git URLs accept https://, ssh://, and git@host:path;\n"
            "      FORGE_ALLOW_UNSAFE_GIT=1 lifts that restriction\n"
            "      registry packages come from FORGE_REGISTRY_URL recipes and\n"
            "      pin their upstream source in Forge.lock\n"
            "  forge remove <NAME> [--manifest PATH]\n"
            "  forge new <NAME>      scaffold a new project directory\n"
            "  forge init            scaffold into the current directory\n"
            "Build options (build/check/run/test/debug):\n"
            "  -j, --jobs N          compile up to N translation units at once ('auto' restores the default)\n"
            "  -q, --quiet           no output except errors and program output\n"
            "  -v                    show per-file compile/link progress\n"
            "  -vv                   also echo every compiler/linker command line\n"
            "  --offline             resolve dependencies without network access\n"
            "  --locked              fail rather than move a Forge.lock pin\n"
            "  --frozen              --offline and --locked together\n"
            "Environment:\n"
            "  FORGE_HOME                    dependency cache root (default ~/.forge)\n"
            "  FORGE_REGISTRY_URL            sunn registry for --registry deps\n"
            "  FORGE_ALLOW_UNSAFE_REGISTRY=1 permit file:// registry URLs\n"
            "  FORGE_DEBUGGER                debugger executable for `forge debug`\n"
            "  FORGE_ALLOW_UNSAFE_GIT=1      permit local-path/file:// git URLs\n"
            "  NO_COLOR                      disable colored status lines\n"
            "See 'forge help <command>' for details on a single command.\n");
}

/* One-line usage plus a sentence of intent for every verb. */
typedef struct ForgeVerbHelp {
    const char *verb;
    const char *usage;
    const char *detail;
} ForgeVerbHelp;

static const ForgeVerbHelp VERB_HELP[] = {
    { "build", "forge build [--release] [--jobs N] [--manifest PATH]",
      "Compile every source and link the final binary into target/." },
    { "check", "forge check [--release] [--jobs N] [--manifest PATH]",
      "Compile only (no link): a fast syntax and type gate." },
    { "run", "forge run [--release] [--jobs N] [--manifest PATH] [-- ARGS...]",
      "Build, then run the binary; arguments after -- go to the program "
      "and its exit code propagates." },
    { "test", "forge test [--release] [--jobs N] [--test NAME] [--manifest PATH]",
      "Build each tests/*.c as a self-contained binary and run it; "
      "--test filters by name." },
    { "debug", "forge debug [--release] [--jobs N] [--manifest PATH]",
      "Build, then run the platform debugger in batch mode with the raw "
      "session post-processed into a stack/disassembly view. Override the "
      "debugger with FORGE_DEBUGGER." },
    { "clean", "forge clean [--manifest PATH]",
      "Remove the target/ artifact tree for this project." },
    { "update", "forge update [NAME] [--offline] [--manifest PATH]",
      "Re-resolve dependencies: bare, every dep moves to the newest allowed "
      "state; naming one moves only that dep past its pin. --offline forbids "
      "network access." },
    { "add", "forge add <NAME> (--git URL | --path DIR | --registry PKG) "
             "[--tag T | --branch B | --rev R] [--version VER] [--manifest PATH]",
      "Insert a dependency into [dependencies]; git URLs must use https://, "
      "ssh://, or git@host:path; --version pins a registry package (latest "
      "when omitted)." },
    { "remove", "forge remove <NAME> [--manifest PATH]",
      "Remove a dependency from [dependencies] and its lock entry." },
    { "new", "forge new <NAME>",
      "Scaffold a new project directory with a ready-to-build manifest." },
    { "init", "forge init",
      "Scaffold a project into the current directory." }
};

static void print_command_help(const char *verb)
{
    size_t index;

    for (index = 0U; index < sizeof(VERB_HELP) / sizeof(VERB_HELP[0]); ++index) {
        if (strcmp(VERB_HELP[index].verb, verb) == 0) {
            printf("%s\n  %s\n", VERB_HELP[index].usage, VERB_HELP[index].detail);
            return;
        }
    }
    fprintf(stderr, "forge: no help entry for '%s'\n", verb);
}

/* Flags that consume the next argv element report a precise error instead
 * of falling through to "unsupported option" when the value is missing. */
static int flag_value_missing(const char *flag, int index, int argc)
{
    if (index + 1 >= argc) {
        fprintf(stderr, "forge: option '%s' requires a value\n", flag);
        return 1;
    }
    return 0;
}

/* Parses a positive integer flag value; returns -1 on a bad value and leaves
 * `output` untouched when N is "auto" or the flag is absent. */
static int parse_jobs(const char *value, int *output)
{
    char *end = NULL;
    long parsed;
    int number;

    if (value == NULL || strcmp(value, "auto") == 0) {
        return 0;
    }
    parsed = strtol(value, &end, 10);
    if (end == value || *end != '\0' || parsed < 1 || parsed > 1024) {
        return -1;
    }
    number = (int)parsed;
    if (number != parsed) {
        return -1;
    }
    *output = number;
    return 0;
}

/* When the caller did not pass --manifest explicitly, search upward from the
 * working directory like Cargo does. Falls back to the plain default name so
 * the normal "could not open manifest" error still guides the user. */
static void discover_manifest(const char **manifest_path, int explicit_manifest,
                              char *storage, size_t storage_size)
{
    if (explicit_manifest) {
        return;
    }
    if (forge_paths_find_manifest("Forge.toml", storage, storage_size) == 1) {
        *manifest_path = storage;
    }
}

/*
 * Applies -q/-v/-vv to the console verbosity immediately (the log layer reads
 * it globally). Returns 1 when the argument was a verbosity flag, so callers
 * can fall through to their own options otherwise.
 */
static int apply_verbosity_flag(const char *argument)
{
    if (strcmp(argument, "-q") == 0 || strcmp(argument, "--quiet") == 0) {
        forge_log_set_verbosity(FORGE_VERBOSITY_QUIET);
    } else if (strcmp(argument, "-v") == 0) {
        forge_log_set_verbosity(FORGE_VERBOSITY_VERBOSE);
    } else if (strcmp(argument, "-vv") == 0) {
        forge_log_set_verbosity(FORGE_VERBOSITY_VERY_VERBOSE);
    } else {
        return 0;
    }
    return 1;
}

static int command_new(int argc, char **argv)
{
    if (argc < 3) {
        fprintf(stderr, "forge: 'new' requires a project name\n");
        print_usage(stderr);
        return 1;
    }
    if (argc > 3) {
        fprintf(stderr, "forge: unsupported option '%s'\n", argv[3]);
        return 1;
    }
    return forge_orchestrate_new(argv[2]);
}

static int command_init(int argc, char **argv)
{
    if (argc > 2) {
        fprintf(stderr, "forge: unsupported option '%s'\n", argv[2]);
        return 1;
    }
    return forge_orchestrate_init();
}

/* Shared parser for the manifest-only command (clean). */
static int command_manifest_only(const char *command, int argc, char **argv,
                                 int (*dispatch)(const char *))
{
    const char *manifest_path = "Forge.toml";
    char discovered[FORGE_PATH_MAX];
    int explicit_manifest = 0;
    int index;

    for (index = 2; index < argc; ++index) {
        if (strcmp(argv[index], "--manifest") == 0) {
            if (flag_value_missing("--manifest", index, argc)) {
                return 1;
            }
            manifest_path = argv[++index];
            explicit_manifest = 1;
        } else {
            fprintf(stderr, "forge: unsupported %s option '%s'\n", command, argv[index]);
            return 1;
        }
    }
    discover_manifest(&manifest_path, explicit_manifest, discovered, sizeof(discovered));
    return dispatch(manifest_path);
}

/* Shared parser for build/check/run/test/debug: profile, job, and policy
 * flags plus --manifest; run additionally accepts "-- ARGS..." for the
 * program itself and test accepts --test NAME to run a single test. */
static int command_build_like(const char *command, int argc, char **argv)
{
    const char *manifest_path = "Forge.toml";
    const char *program_arguments[FORGE_MAX_PROGRAM_ARGUMENTS];
    ForgeBuildOptions options = forge_build_default_options();
    char discovered[FORGE_PATH_MAX];
    const char *test_filter = NULL;
    size_t program_argument_count = 0U;
    int accepts_program_arguments = strcmp(command, "run") == 0;
    int explicit_manifest = 0;
    int index;

    for (index = 2; index < argc; ++index) {
        if (accepts_program_arguments && strcmp(argv[index], "--") == 0) {
            while (++index < argc) {
                if (program_argument_count == FORGE_MAX_PROGRAM_ARGUMENTS) {
                    fprintf(stderr, "forge: too many program arguments (max %u)\n",
                            (unsigned int)FORGE_MAX_PROGRAM_ARGUMENTS);
                    return 1;
                }
                program_arguments[program_argument_count++] = argv[index];
            }
            break;
        }
        if (apply_verbosity_flag(argv[index])) {
            continue;
        }
        if (strcmp(argv[index], "--release") == 0) {
            options.release = 1;
        } else if (strcmp(argv[index], "--offline") == 0) {
            options.offline = 1;
        } else if (strcmp(argv[index], "--locked") == 0) {
            options.locked = 1;
        } else if (strcmp(argv[index], "--frozen") == 0) {
            options.offline = 1;
            options.locked = 1;
        } else if (strcmp(argv[index], "--test") == 0) {
            if (strcmp(command, "test") != 0) {
                fprintf(stderr, "forge: unsupported %s option '%s'\n",
                        command, argv[index]);
                return 1;
            }
            if (flag_value_missing("--test", index, argc)) {
                return 1;
            }
            test_filter = argv[++index];
        } else if (strcmp(argv[index], "--manifest") == 0) {
            if (flag_value_missing("--manifest", index, argc)) {
                return 1;
            }
            manifest_path = argv[++index];
            explicit_manifest = 1;
        } else if (strcmp(argv[index], "--jobs") == 0 ||
                   strcmp(argv[index], "-j") == 0) {
            if (flag_value_missing(argv[index], index, argc)) {
                return 1;
            }
            ++index;
            if (parse_jobs(argv[index], &options.max_jobs) != 0) {
                fprintf(stderr, "forge: '%s' expects a positive integer or 'auto'\n",
                        argv[index - 1]);
                return 1;
            }
        } else {
            fprintf(stderr, "forge: unsupported %s option '%s'\n",
                    command, argv[index]);
            return 1;
        }
    }
    discover_manifest(&manifest_path, explicit_manifest, discovered, sizeof(discovered));

    if (strcmp(command, "build") == 0) {
        return forge_orchestrate_build(manifest_path, &options);
    }
    if (strcmp(command, "check") == 0) {
        return forge_orchestrate_check(manifest_path, &options);
    }
    if (strcmp(command, "test") == 0) {
        return forge_orchestrate_test(manifest_path, &options, test_filter);
    }
    if (strcmp(command, "debug") == 0) {
        return forge_orchestrate_debug(manifest_path, &options);
    }
    return forge_orchestrate_run(manifest_path, &options,
                                 program_arguments, program_argument_count);
}

/* forge add NAME (--git URL | --path DIR | --registry PKG)
 * [--tag T|--branch B|--rev R] [--version VER] [--manifest PATH].
 * Exactly one source; refs are git-only, versions registry-only. */
static int command_add(int argc, char **argv)
{
    const char *manifest_path = "Forge.toml";
    const char *name = NULL;
    const char *git_url = NULL;
    const char *dep_path = NULL;
    const char *registry_package = NULL;
    const char *registry_version = "";
    const char *ref_kind = "";
    const char *ref_value = "";
    char discovered[FORGE_PATH_MAX];
    int explicit_manifest = 0;
    int index;

    if (argc < 3 || argv[2][0] == '-') {
        fprintf(stderr,
                "forge: 'add' requires a dependency name, e.g. "
                "forge add mylib --git https://example.com/mylib\n");
        return 1;
    }
    name = argv[2];
    for (index = 3; index < argc; ++index) {
        if (strcmp(argv[index], "--git") == 0) {
            if (flag_value_missing("--git", index, argc)) {
                return 1;
            }
            git_url = argv[++index];
        } else if (strcmp(argv[index], "--path") == 0) {
            if (flag_value_missing("--path", index, argc)) {
                return 1;
            }
            dep_path = argv[++index];
        } else if (strcmp(argv[index], "--registry") == 0) {
            if (flag_value_missing("--registry", index, argc)) {
                return 1;
            }
            registry_package = argv[++index];
        } else if (strcmp(argv[index], "--version") == 0) {
            if (flag_value_missing("--version", index, argc)) {
                return 1;
            }
            registry_version = argv[++index];
        } else if (strcmp(argv[index], "--tag") == 0 ||
                   strcmp(argv[index], "--branch") == 0 ||
                   strcmp(argv[index], "--rev") == 0) {
            if (flag_value_missing(argv[index], index, argc)) {
                return 1;
            }
            if (ref_value[0] != '\0') {
                fprintf(stderr, "forge: 'add' accepts only one of "
                                "--tag/--branch/--rev\n");
                return 1;
            }
            ref_kind = strcmp(argv[index], "--tag") == 0 ? "tag" :
                       strcmp(argv[index], "--branch") == 0 ? "branch" : "rev";
            ref_value = argv[++index];
        } else if (strcmp(argv[index], "--manifest") == 0) {
            if (flag_value_missing("--manifest", index, argc)) {
                return 1;
            }
            manifest_path = argv[++index];
            explicit_manifest = 1;
        } else {
            fprintf(stderr, "forge: unsupported add option '%s'\n", argv[index]);
            return 1;
        }
    }
    discover_manifest(&manifest_path, explicit_manifest, discovered, sizeof(discovered));
    return forge_orchestrate_add(manifest_path, name, git_url != NULL ? git_url : "",
                                 ref_kind, ref_value, dep_path != NULL ? dep_path : "",
                                 registry_package != NULL ? registry_package : "",
                                 registry_version);
}

static int command_remove(int argc, char **argv)
{
    const char *manifest_path = "Forge.toml";
    char discovered[FORGE_PATH_MAX];
    int explicit_manifest = 0;
    int index;

    if (argc < 3 || argv[2][0] == '-') {
        fprintf(stderr, "forge: 'remove' requires a dependency name\n");
        return 1;
    }
    for (index = 3; index < argc; ++index) {
        if (strcmp(argv[index], "--manifest") == 0) {
            if (flag_value_missing("--manifest", index, argc)) {
                return 1;
            }
            manifest_path = argv[++index];
            explicit_manifest = 1;
        } else {
            fprintf(stderr, "forge: unsupported remove option '%s'\n", argv[index]);
            return 1;
        }
    }
    discover_manifest(&manifest_path, explicit_manifest, discovered, sizeof(discovered));
    return forge_orchestrate_remove(manifest_path, argv[2]);
}

/* forge update [NAME] [--offline] [--manifest PATH]: without NAME every
 * dependency re-resolves to the newest allowed state; with NAME only that
 * dependency is pulled past its lock pin and the rest stay quiet and
 * offline-friendly. --offline keeps the re-resolution network-free; --locked
 * and --frozen are rejected because update's whole job is rewriting pins. */
static int command_update(int argc, char **argv)
{
    const char *manifest_path = "Forge.toml";
    const char *only_name = NULL;
    char discovered[FORGE_PATH_MAX];
    int explicit_manifest = 0;
    int offline = 0;
    int index;

    for (index = 2; index < argc; ++index) {
        if (apply_verbosity_flag(argv[index])) {
            continue;
        }
        if (strcmp(argv[index], "--locked") == 0 ||
            strcmp(argv[index], "--frozen") == 0) {
            fprintf(stderr,
                    "forge: '%s' contradicts update, whose job is rewriting "
                    "Forge.lock pins; use it on build/check/run/test/debug\n",
                    argv[index]);
            return 1;
        } else if (strcmp(argv[index], "--offline") == 0) {
            offline = 1;
        } else if (strcmp(argv[index], "--manifest") == 0) {
            if (flag_value_missing("--manifest", index, argc)) {
                return 1;
            }
            manifest_path = argv[++index];
            explicit_manifest = 1;
        } else if (argv[index][0] != '-' && only_name == NULL) {
            only_name = argv[index];
        } else if (argv[index][0] != '-') {
            fprintf(stderr, "forge: 'update' accepts at most one dependency "
                            "name\n");
            return 1;
        } else {
            fprintf(stderr, "forge: unsupported update option '%s'\n", argv[index]);
            return 1;
        }
    }
    discover_manifest(&manifest_path, explicit_manifest, discovered, sizeof(discovered));
    return forge_orchestrate_update(manifest_path, only_name, offline);
}

int forge_cli_main(int argc, char **argv)
{
    const char *command;
    int wants_help;

    if (argc == 1 || strcmp(argv[1], "--help") == 0 ||
        strcmp(argv[1], "-h") == 0 || strcmp(argv[1], "help") == 0) {
        /* `forge help <verb>` (and `forge help -h` etc.) prints one verb. */
        if (argc == 3 && argv[2][0] != '-') {
            print_command_help(argv[2]);
            return 0;
        }
        print_usage(stdout);
        return 0;
    }
    if (strcmp(argv[1], "--version") == 0 || strcmp(argv[1], "-V") == 0) {
        printf("forge %s\n", FORGE_VERSION);
        return 0;
    }
    command = argv[1];
    if (strcmp(command, "build") != 0 && strcmp(command, "check") != 0 &&
        strcmp(command, "run") != 0 && strcmp(command, "test") != 0 &&
        strcmp(command, "debug") != 0 && strcmp(command, "clean") != 0 &&
        strcmp(command, "update") != 0 && strcmp(command, "add") != 0 &&
        strcmp(command, "remove") != 0 &&
        strcmp(command, "new") != 0 && strcmp(command, "init") != 0) {
        fprintf(stderr, "forge: unsupported command '%s'\n", argv[1]);
        print_usage(stderr);
        return 1;
    }
    /* Per-verb help: `forge build --help` / `forge build -h`. */
    wants_help = argc == 3 && (strcmp(argv[2], "--help") == 0 ||
                               strcmp(argv[2], "-h") == 0);
    if (wants_help) {
        print_command_help(command);
        return 0;
    }
    if (strcmp(command, "new") == 0) {
        return command_new(argc, argv);
    }
    if (strcmp(command, "init") == 0) {
        return command_init(argc, argv);
    }
    if (strcmp(command, "clean") == 0) {
        return command_manifest_only(command, argc, argv, forge_orchestrate_clean);
    }
    if (strcmp(command, "update") == 0) {
        return command_update(argc, argv);
    }
    if (strcmp(command, "add") == 0) {
        return command_add(argc, argv);
    }
    if (strcmp(command, "remove") == 0) {
        return command_remove(argc, argv);
    }
    return command_build_like(command, argc, argv);
}
