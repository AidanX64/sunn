#ifndef FORGE_COMPILER_H
#define FORGE_COMPILER_H

#include <stddef.h>

#include "forge/argv.h"
#include "forge/manifest.h"

#define FORGE_COMPILER_VALUE_MAX 256U
#define FORGE_COMMAND_MAX 8192U

typedef enum ForgeHostOs {
    FORGE_HOST_OS_WINDOWS,
    FORGE_HOST_OS_LINUX,
    FORGE_HOST_OS_MACOS,
    FORGE_HOST_OS_UNKNOWN
} ForgeHostOs;

typedef enum ForgeCompilerKind {
    FORGE_COMPILER_MSVC,
    FORGE_COMPILER_GCC,
    FORGE_COMPILER_CLANG
} ForgeCompilerKind;

typedef enum ForgeSourceLanguage {
    FORGE_SOURCE_C,
    FORGE_SOURCE_CPP,
    FORGE_SOURCE_ASM
} ForgeSourceLanguage;

typedef struct ForgeHostInfo {
    ForgeHostOs os;
    char os_name[FORGE_COMPILER_VALUE_MAX];
    char version[FORGE_COMPILER_VALUE_MAX];
} ForgeHostInfo;

typedef struct ForgeCompiler {
    ForgeCompilerKind kind;
    char program[FORGE_COMPILER_VALUE_MAX];
    int used_fallback;
    char selection_note[FORGE_COMPILER_VALUE_MAX];
} ForgeCompiler;

/* Identifies the operating system on which Forge is building. */
int forge_detect_host(ForgeHostInfo *host, char *error, size_t error_size);

/*
 * Selects a locally invocable compiler. An explicit override is honored first.
 * For an unknown host Forge selects clang deliberately and reports that choice
 * through selection_note; an unavailable fallback is reported as an error.
 */
int forge_compiler_select(const ForgeHostInfo *host, const char *override_program,
                          ForgeCompiler *compiler, char *error, size_t error_size);

/*
 * Computes the dependency file path paired with an object file (the object
 * path with ".o" swapped for ".d"). GCC/Clang write the headers a
 * translation unit actually used into it via -MMD -MF, and Forge reads it
 * back to decide whether a rebuild is still needed.
 */
int forge_compiler_depfile_path(const char *object_path, char *depfile_path,
                                size_t depfile_size);

/* Builds the argv for compiling one source file to one object file. Extra
 * include directories (dependency headers) are appended after the project's
 * own include directory in the compiler's dialect. A non-empty
 * `project_version` is injected as FORGE_PROJECT_VERSION ("x.y.z") into every
 * C/C++ translation unit (assembly is left untouched). */
int forge_compiler_make_compile_argv(const ForgeCompiler *compiler,
                                     ForgeSourceLanguage language,
                                     const char *source_path,
                                     const char *object_path,
                                     const char *project_root,
                                     const char *target_os,
                                     const char *target_arch,
                                     const ForgeBuildProfile *profile,
                                     const ForgeStringList *extra_include_dirs,
                                     const char *project_version,
                                     ForgeArgv *argv,
                                     char *error, size_t error_size);

/*
 * Builds the argv for linking object files into an executable. Static
 * libraries and dependency objects (`extra_link_inputs`) are appended after
 * the project's own objects. When the flattened command line would exceed
 * what the OS accepts, the object list and flags are spilled into a compiler
 * response file in `response_dir` and the compiler is invoked with @<file>;
 * *used_response_file reports whether that happened so callers can log and
 * clean up.
 */
int forge_compiler_make_link_argv(const ForgeCompiler *compiler,
                                  int has_cpp_source,
                                  const char *const *object_paths,
                                  size_t object_count,
                                  const char *const *extra_link_inputs,
                                  size_t extra_link_input_count,
                                  const char *output_path,
                                  const char *response_dir,
                                  const ForgeBuildProfile *profile,
                                  ForgeArgv *argv,
                                  int *used_response_file,
                                  char *error, size_t error_size);

const char *forge_host_os_name(ForgeHostOs os);
const char *forge_compiler_kind_name(ForgeCompilerKind kind);

#endif
