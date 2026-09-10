#include <stdio.h>
#include <string.h>

#include "forge/argv.h"
#include "forge/compiler.h"
#include "forge/flags.h"
#include "forge/manifest.h"

static int is_msvc(const ForgeCompiler *compiler)
{
    return compiler != NULL && compiler->kind == FORGE_COMPILER_MSVC;
}

static int append_opt_level(ForgeArgv *argv, const ForgeCompiler *compiler,
                            int level, int for_link)
{
    const char *flag;

    switch (level) {
    case 0:
        flag = is_msvc(compiler) ? "/Od" : "-O0";
        break;
    case 1:
        flag = is_msvc(compiler) ? "/O1" : "-O1";
        break;
    case 2:
        flag = is_msvc(compiler) ? "/O2" : "-O2";
        break;
    case 3:
        flag = is_msvc(compiler) ? "/O2" : "-O3";
        break;
    default:
        return 0; /* unset: compiler default */
    }
    if (is_msvc(compiler) && for_link) {
        return 0; /* /O* is a compile-stage option */
    }
    return forge_argv_append(argv, flag);
}

static int append_debug_info(ForgeArgv *argv, const ForgeCompiler *compiler,
                             int debug_info, int for_link)
{
    if (debug_info <= 0) {
        return 0;
    }
    if (is_msvc(compiler)) {
        return forge_argv_append(argv, for_link ? "/DEBUG" : "/Zi");
    }
    return forge_argv_append(argv, "-g");
}

static int append_std_gnu(ForgeArgv *argv, const char *std_version, size_t std_size)
{
    char flag[FORGE_MANIFEST_VALUE_MAX + 8U];

    if (std_version[0] == '\0' || std_size == 0U) {
        return 0;
    }
    if ((size_t)snprintf(flag, sizeof(flag), "-std=%s", std_version) >= sizeof(flag)) {
        return -1;
    }
    return forge_argv_append(argv, flag);
}

static int append_std_msvc(ForgeArgv *argv, const char *std_version,
                           char *error, size_t error_size)
{
    const char *flag;

    if (strcmp(std_version, "c11") == 0) {
        flag = "/std:c11";
    } else if (strcmp(std_version, "c17") == 0) {
        flag = "/std:c17";
    } else if (strcmp(std_version, "c++14") == 0) {
        flag = "/std:c++14";
    } else if (strcmp(std_version, "c++17") == 0) {
        flag = "/std:c++17";
    } else if (strcmp(std_version, "c++20") == 0) {
        flag = "/std:c++20";
    } else if (strcmp(std_version, "c++23") == 0) {
        flag = "/std:c++23";
    } else {
        if (error != NULL && error_size != 0U) {
            (void)snprintf(error, error_size,
                           "standard '%s' is not supported by MSVC; "
                           "choose c11, c17, c++14, c++17, c++20, or c++23",
                           std_version);
        }
        return -1;
    }
    return forge_argv_append(argv, flag);
}

/* Translates one GCC/Clang-dialect cflag for the MSVC toolchain.
 * Returns 0 when the flag was translated or deliberately dropped, and
 * -1 when it has no MSVC equivalent (rejected with a diagnostic). */
static int translate_cflag_msvc(ForgeArgv *argv, const char *flag,
                                char *error, size_t error_size)
{
    const char *mapped = NULL;

    if (strcmp(flag, "-Wall") == 0 || strcmp(flag, "-Wextra") == 0 ||
        strcmp(flag, "-Wpedantic") == 0 || strcmp(flag, "-pedantic") == 0) {
        mapped = "/W4";
        return forge_argv_append(argv, mapped);
    }
    if (strcmp(flag, "-Werror") == 0) {
        mapped = "/WX";
        return forge_argv_append(argv, mapped);
    }
    if (strcmp(flag, "-O0") == 0) {
        mapped = "/Od";
    } else if (strcmp(flag, "-O1") == 0) {
        mapped = "/O1";
    } else if (strcmp(flag, "-O2") == 0) {
        mapped = "/O2";
    } else if (strcmp(flag, "-O3") == 0) {
        mapped = "/O2";
    } else if (strcmp(flag, "-Os") == 0) {
        mapped = "/Os";
    } else if (strcmp(flag, "-Ofast") == 0) {
        mapped = "/Ox";
    } else if (strcmp(flag, "-O") == 0) {
        mapped = "/O1";
    } else if (strcmp(flag, "-g") == 0 || strcmp(flag, "-ggdb") == 0) {
        mapped = "/Zi";
    } else if (strncmp(flag, "-ggdb", 5U) == 0 && flag[5] >= '1' && flag[5] <= '3') {
        mapped = "/Zi";
    } else if (strncmp(flag, "-g", 2U) == 0 && flag[2] >= '1' && flag[2] <= '3') {
        mapped = "/Zi";
    } else if (strcmp(flag, "-g0") == 0 || strcmp(flag, "-ggdb0") == 0) {
        return 0; /* explicit "no debug info": nothing to emit */
    } else if (strncmp(flag, "-std=", 5U) == 0) {
        return append_std_msvc(argv, flag + 5, error, error_size);
    } else if (strncmp(flag, "-D", 2U) == 0 || strncmp(flag, "-U", 2U) == 0) {
        return forge_argv_appendf(argv, "/%s", flag + 1);
    } else if (strncmp(flag, "-I", 2U) == 0) {
        return forge_argv_appendf(argv, "/I%s", flag + 2);
    }
    if (mapped != NULL) {
        return forge_argv_append(argv, mapped);
    }
    if (error != NULL && error_size != 0U) {
        (void)snprintf(error, error_size,
                       "cannot translate GCC/Clang flag '%s' for MSVC; there may "
                       "be no equivalent. Prefer the portable [profile] fields "
                       "(opt-level, debug, warnings-as-errors, std) or remove the "
                       "flag",
                       flag);
    }
    return -1;
}

int forge_flags_append(ForgeArgv *argv, const ForgeCompiler *compiler,
                       const ForgeBuildProfile *profile, int for_link,
                       char *error, size_t error_size)
{
    size_t index;

    if (argv == NULL || compiler == NULL || profile == NULL ||
        error == NULL || error_size == 0U) {
        return -1;
    }
    if (append_opt_level(argv, compiler, profile->opt_level, for_link) != 0 ||
        append_debug_info(argv, compiler, profile->debug_info, for_link) != 0) {
        return -1;
    }
    if (!for_link) {
        if (compiler->kind == FORGE_COMPILER_MSVC) {
            if (append_std_msvc(argv, profile->std_version, error, error_size) != 0) {
                return -1;
            }
        } else if (append_std_gnu(argv, profile->std_version,
                                  sizeof(profile->std_version)) != 0) {
            return -1;
        }
        /* Warning and raw-flag options are compile-stage only for MSVC: the
         * cl-driven link step rejects /W* and friends with LNK4044 noise.
         * GCC/Clang drivers accept them at link time, so their cflags stay
         * on both stages (that is how -lm-style flags reach the linker). */
        if (compiler->kind == FORGE_COMPILER_MSVC) {
            for (index = 0U; index < profile->cflags.count; ++index) {
                int translated = translate_cflag_msvc(argv, profile->cflags.items[index],
                                                      error, error_size);

                if (translated < 0) {
                    return -1;
                }
            }
            if (profile->warnings_as_errors &&
                forge_argv_append(argv, "/WX") != 0) {
                return -1;
            }
        } else {
            if (profile->warnings_as_errors && forge_argv_append(argv, "-Werror") != 0) {
                return -1;
            }
            for (index = 0U; index < profile->cflags.count; ++index) {
                if (forge_argv_append(argv, profile->cflags.items[index]) != 0) {
                    return -1;
                }
            }
        }
        return 0;
    }

    /* Link stage beyond the /O* //DEBUG pair handled above:
     * - MSVC gets nothing else (everything left is invalid to link.exe);
     * - GCC/Clang get the portable warning flag plus raw cflags so link-time
     *   options like library names still apply. */
    if (compiler->kind == FORGE_COMPILER_MSVC) {
        return 0;
    }
    if (profile->warnings_as_errors && forge_argv_append(argv, "-Werror") != 0) {
        return -1;
    }
    for (index = 0U; index < profile->cflags.count; ++index) {
        if (forge_argv_append(argv, profile->cflags.items[index]) != 0) {
            return -1;
        }
    }
    return 0;
}