#include <ctype.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "forge/platform.h"

#if FORGE_PLATFORM_WINDOWS
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
/*
 * RtlGetVersion is declared in winternl.h on some SDKs but not all (it is
 * missing from the 10.0.26100 Windows SDK). Resolve it at runtime from ntdll
 * instead, so no extra link dependency is introduced for host OS detection.
 */
typedef struct ForgeRtlOsVersionInfo {
    ULONG dwOSVersionInfoSize;
    ULONG dwMajorVersion;
    ULONG dwMinorVersion;
    ULONG dwBuildNumber;
    ULONG dwPlatformId;
    WCHAR szCSDVersion[128];
} ForgeRtlOsVersionInfo;
typedef LONG (WINAPI *ForgeRtlGetVersionFn)(ForgeRtlOsVersionInfo *);

static LONG rtl_get_version(ForgeRtlOsVersionInfo *version)
{
    HMODULE ntdll;
    ForgeRtlGetVersionFn get_version;

    if (version == NULL) {
        return -1;
    }
    ntdll = GetModuleHandleW(L"ntdll.dll");
    if (ntdll == NULL) {
        return -1;
    }
    get_version = (ForgeRtlGetVersionFn)(void *)GetProcAddress(ntdll, "RtlGetVersion");
    if (get_version == NULL) {
        return -1;
    }
    return get_version(version);
}
#elif defined(__linux__)
#include <sys/utsname.h>
#elif defined(__APPLE__)
#include <sys/types.h>
#include <sys/sysctl.h>
#endif

#include "forge/compiler.h"
#include "forge/flags.h"
#include "forge/process.h"
#include "forge_util.h"

#define FORGE_PATH_MAX_LOCAL 1024U

/* Case-insensitive compare of `text` against the lowercase `literal`. */
static int text_equals_case_insensitive(const char *text, const char *literal)
{
    size_t index;

    for (index = 0U; text[index] != '\0'; ++index) {
        if (tolower((unsigned char)text[index]) !=
            (unsigned char)literal[index]) {
            return 0;
        }
    }
    return literal[index] == '\0';
}

/*
 * Classifies an override program by its file name rather than by substring
 * scans of the whole string: a path like C:\Users\chloe\bin\gcc.exe must stay
 * a GCC driver even though it happens to contain "cl". clang keeps substring
 * matching because clang-cl, versioned drivers, and absolute toolchain paths
 * legitimately carry "clang" anywhere in the spelling.
 */
static ForgeCompilerKind compiler_kind_from_program(const char *program)
{
    const char *base = program;
    const char *cursor;
    size_t length;

    if (strstr(program, "clang") != NULL) {
        return FORGE_COMPILER_CLANG;
    }
    for (cursor = program; *cursor != '\0'; ++cursor) {
        if (*cursor == '/' || *cursor == '\\') {
            base = cursor + 1;
        }
    }
    length = strlen(base);
    if (length > 4U &&
        text_equals_case_insensitive(base + length - 4U, ".exe")) {
        length -= 4U;
    }
    if (length == 2U && tolower((unsigned char)base[0]) == 'c' &&
        tolower((unsigned char)base[1]) == 'l') {
        return FORGE_COMPILER_MSVC;
    }
    return FORGE_COMPILER_GCC;
}


const char *forge_host_os_name(ForgeHostOs os)
{
    switch (os) {
    case FORGE_HOST_OS_WINDOWS:
        return "windows";
    case FORGE_HOST_OS_LINUX:
        return "linux";
    case FORGE_HOST_OS_MACOS:
        return "macos";
    case FORGE_HOST_OS_UNKNOWN:
        return "unknown";
    }
    return "unknown";
}

const char *forge_compiler_kind_name(ForgeCompilerKind kind)
{
    switch (kind) {
    case FORGE_COMPILER_MSVC:
        return "msvc";
    case FORGE_COMPILER_GCC:
        return "gcc";
    case FORGE_COMPILER_CLANG:
        return "clang";
    }
    return "unknown";
}

int forge_detect_host(ForgeHostInfo *host, char *error, size_t error_size)
{
    if (host == NULL) {
        forge_util_set_error(error, error_size, "host output is required");
        return -1;
    }
    *host = (ForgeHostInfo){0};

#if FORGE_PLATFORM_WINDOWS
    {
        ForgeRtlOsVersionInfo version = {0};
        version.dwOSVersionInfoSize = sizeof(version);
        host->os = FORGE_HOST_OS_WINDOWS;
        (void)snprintf(host->os_name, sizeof(host->os_name), "windows");
        /*
         * GetVersionExA is deprecated and reports a skewed Windows 10/11
         * build number unless the binary embeds a compatibility manifest.
         * RtlGetVersion returns the real OS version and is not deprecated.
         */
        if (rtl_get_version(&version) != 0 || version.dwMajorVersion == 0U) {
            (void)snprintf(host->version, sizeof(host->version), "unknown");
        } else {
            (void)snprintf(host->version, sizeof(host->version), "%lu.%lu.%lu",
                           (unsigned long)version.dwMajorVersion,
                           (unsigned long)version.dwMinorVersion,
                           (unsigned long)version.dwBuildNumber);
        }
    }
#elif defined(__linux__)
    {
        struct utsname details;
        host->os = FORGE_HOST_OS_LINUX;
        (void)snprintf(host->os_name, sizeof(host->os_name), "linux");
        if (uname(&details) != 0) {
            (void)snprintf(host->version, sizeof(host->version), "unknown");
        } else {
            (void)snprintf(host->version, sizeof(host->version), "%s", details.release);
        }
    }
#elif defined(__APPLE__)
    {
        size_t version_size = sizeof(host->version);
        host->os = FORGE_HOST_OS_MACOS;
        (void)snprintf(host->os_name, sizeof(host->os_name), "macos");
        /*
         * sysctlbyname is deprecated in macOS 12+ and would otherwise trip
         * -Werror. Suppress the warning just for this lookup; it still works.
         */
#if defined(__clang__)
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wdeprecated-declarations"
#endif
        if (sysctlbyname("kern.osproductversion", host->version, &version_size,
                         NULL, 0U) != 0) {
            (void)snprintf(host->version, sizeof(host->version), "unknown");
        }
#if defined(__clang__)
#pragma clang diagnostic pop
#endif
    }
#else
    host->os = FORGE_HOST_OS_UNKNOWN;
    (void)snprintf(host->os_name, sizeof(host->os_name), "unknown");
    (void)snprintf(host->version, sizeof(host->version), "unknown");
#endif
    return 0;
}

static int select_program(const char *program, ForgeCompilerKind kind, int fallback,
                          const char *note, ForgeCompiler *compiler,
                          char *error, size_t error_size)
{
    if (!forge_util_program_available(program)) {
        forge_util_set_error(error, error_size, "%s; compiler '%s' is not available on PATH",
                  note, program);
        return -1;
    }
    compiler->kind = kind;
    compiler->used_fallback = fallback;
    (void)snprintf(compiler->program, sizeof(compiler->program), "%s", program);
    (void)snprintf(compiler->selection_note, sizeof(compiler->selection_note), "%s",
                   note);
    return 0;
}

/*
 * MSVC's cl.exe is only usable when the Visual Studio environment has been
 * initialized (INCLUDE/LIB variables must point at the SDK and CRT headers).
 * A bare cl.exe on PATH will error with C1083 on every system header, so
 * treat it as unavailable unless its environment is actually set up.
 */
static int msvc_environment_ready(void)
{
#if FORGE_PLATFORM_WINDOWS
    const char *include = getenv("INCLUDE");
    return include != NULL && include[0] != '\0';
#else
    return 0;
#endif
}

int forge_compiler_select(const ForgeHostInfo *host, const char *override_program,
                          ForgeCompiler *compiler, char *error, size_t error_size)
{
    if (host == NULL || compiler == NULL) {
        forge_util_set_error(error, error_size, "host and compiler output are required");
        return -1;
    }
    *compiler = (ForgeCompiler){0};

    if (override_program != NULL && override_program[0] != '\0') {
        return select_program(override_program, compiler_kind_from_program(override_program),
                              0, "using manifest compiler override", compiler,
                              error, error_size);
    }

    switch (host->os) {
    case FORGE_HOST_OS_WINDOWS:
        if (msvc_environment_ready() && forge_util_program_available("cl")) {
            return select_program("cl", FORGE_COMPILER_MSVC, 0,
                                  "using Windows native compiler", compiler,
                                  error, error_size);
        }
        if (forge_util_program_available("gcc")) {
            return select_program("gcc", FORGE_COMPILER_GCC, 1,
                                  "MSVC was not usable; using gcc fallback", compiler,
                                  error, error_size);
        }
        return select_program("clang", FORGE_COMPILER_CLANG, 1,
                              "MSVC and gcc were not usable; using clang fallback",
                              compiler, error, error_size);
    case FORGE_HOST_OS_LINUX:
        if (forge_util_program_available("gcc")) {
            return select_program("gcc", FORGE_COMPILER_GCC, 0,
                                  "using Linux native compiler", compiler,
                                  error, error_size);
        }
        return select_program("clang", FORGE_COMPILER_CLANG, 1,
                              "gcc was not found; using clang fallback", compiler,
                              error, error_size);
    case FORGE_HOST_OS_MACOS:
        return select_program("clang", FORGE_COMPILER_CLANG, 0,
                              "using macOS native compiler", compiler,
                              error, error_size);
    case FORGE_HOST_OS_UNKNOWN:
        return select_program("clang", FORGE_COMPILER_CLANG, 1,
                              "unsupported host OS; explicitly using clang fallback",
                              compiler, error, error_size);
    }

    forge_util_set_error(error, error_size, "invalid detected host OS");
    return -1;
}

/*
 * Picks the driver that links C++ correctly: the plain gcc/clang drivers do
 * not pull in the C++ standard library, only their g++/clang++ counterparts
 * do. Versioned programs map along ("gcc-13" -> "g++-13", written into
 * `buffer`); anything already carrying "++", or unrecognizable, is passed
 * through unchanged.
 */
static const char *cpp_driver(const ForgeCompiler *compiler,
                              char *buffer, size_t buffer_size)
{
    const char *program = compiler->program;
    const char *prefix = NULL;
    const char *rest = NULL;

    if (strstr(program, "++") == NULL) {
        if (strncmp(program, "gcc", 3U) == 0) {
            prefix = "g++";
            rest = program + 3U;
        } else if (strncmp(program, "clang", 5U) == 0) {
            prefix = "clang++";
            rest = program + 5U;
        }
    }
    if (prefix == NULL) {
        return program;
    }
    (void)snprintf(buffer, buffer_size, "%s%s", prefix, rest);
    return buffer;
}

int forge_compiler_depfile_path(const char *object_path, char *depfile_path,
                                size_t depfile_size)
{
    const char *dot;
    size_t base_length;

    if (object_path == NULL || depfile_path == NULL || depfile_size == 0U) {
        return -1;
    }
    dot = strrchr(object_path, '.');
    base_length = dot == NULL ? strlen(object_path) : (size_t)(dot - object_path);
    if (base_length + 2U >= depfile_size ||
        snprintf(depfile_path, depfile_size, "%.*s.d", (int)base_length, object_path) < 0) {
        return -1;
    }
    return 0;
}

/* Appends the project's own include directory plus any dependency include
 * directories, translated into the compiler's dialect. */
static int append_include_dirs(ForgeArgv *argv, const ForgeCompiler *compiler,
                               const char *project_root,
                               const ForgeStringList *extra_include_dirs,
                               char *error, size_t error_size)
{
    size_t index;

    if (compiler->kind == FORGE_COMPILER_MSVC) {
        if (forge_argv_appendf(argv, "/I%s/include", project_root) != 0) {
            goto out_of_memory;
        }
    } else if (forge_argv_appendf(argv, "-I%s/include", project_root) != 0) {
        goto out_of_memory;
    }
    if (extra_include_dirs != NULL) {
        for (index = 0U; index < extra_include_dirs->count; ++index) {
            if (compiler->kind == FORGE_COMPILER_MSVC) {
                if (forge_argv_appendf(argv, "/I%s",
                                       extra_include_dirs->items[index]) != 0) {
                    goto out_of_memory;
                }
            } else if (forge_argv_appendf(argv, "-I%s",
                                          extra_include_dirs->items[index]) != 0) {
                goto out_of_memory;
            }
        }
    }
    return 0;
out_of_memory:
    forge_util_set_error(error, error_size, "out of memory while building compile command");
    return -1;
}

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
                                     char *error, size_t error_size)
{
    const char *program;
    char cpp_program[FORGE_COMPILER_VALUE_MAX];
    char depfile_path[FORGE_PATH_MAX_LOCAL];

    if (compiler == NULL || source_path == NULL || object_path == NULL ||
        project_root == NULL || target_os == NULL || target_arch == NULL ||
        profile == NULL || argv == NULL) {
        forge_util_set_error(error, error_size, "compiler, paths, target, profile, and argv output are required");
        return -1;
    }
    if (argv->count != 0U) {
        forge_util_set_error(error, error_size, "compile argv output must be empty");
        return -1;
    }
    if (source_path[0] == '\0' || object_path[0] == '\0' || project_root[0] == '\0' ||
        target_os[0] == '\0' || target_arch[0] == '\0') {
        forge_util_set_error(error, error_size, "source, object, project root, target OS, and target architecture are required");
        return -1;
    }

    if (compiler->kind == FORGE_COMPILER_MSVC && language == FORGE_SOURCE_ASM) {
        program = strcmp(target_arch, "x86_64") == 0 ? "ml64" : "ml";
        if (!forge_util_program_available(program)) {
            forge_util_set_error(error, error_size, "MSVC assembly requires '%s' on PATH", program);
            forge_argv_free(argv);
            return -1;
        }
        if (forge_argv_append(argv, program) != 0 ||
            forge_argv_append(argv, "/nologo") != 0 ||
            forge_argv_append(argv, "/c") != 0 ||
            forge_argv_appendf(argv, "/Fo%s", object_path) != 0 ||
            forge_argv_append(argv, source_path) != 0) {
            forge_util_set_error(error, error_size, "out of memory while building compile command");
            forge_argv_free(argv);
            return -1;
        }
    } else if (language == FORGE_SOURCE_ASM && forge_util_has_suffix(source_path, ".asm")) {
        /*
         * GCC/Clang drive the source with their own assembler, which
         * understands .s/.S but not the MSVC-flavoured .asm dialect.
         */
        forge_util_set_error(error, error_size,
                             "the '%s' compiler cannot assemble '.asm' files; "
                             "use .s/.S with GCC/Clang, or the MSVC toolchain for .asm",
                             compiler->program);
        return -1;
    } else if (compiler->kind == FORGE_COMPILER_MSVC) {
        /*
         * MSVC has no -MMD equivalent; tracking its headers would mean
         * parsing the localized /showIncludes output, so no dependency file
         * is requested and freshness falls back to the source mtime only.
         */
        if (forge_argv_append(argv, compiler->program) != 0 ||
            forge_argv_append(argv, "/nologo") != 0 ||
            append_include_dirs(argv, compiler, project_root, extra_include_dirs,
                                error, error_size) != 0 ||
            forge_argv_append(argv, "/c") != 0 ||
            forge_argv_appendf(argv, "/Fo%s", object_path) != 0 ||
            forge_argv_append(argv, source_path) != 0) {
            forge_util_set_error(error, error_size, "out of memory while building compile command");
            forge_argv_free(argv);
            return -1;
        }
    } else if (language == FORGE_SOURCE_ASM) {
        /* Plain assembly has no headers to record in a dependency file. */
        program = compiler->program;
        if (forge_argv_append(argv, program) != 0 ||
            append_include_dirs(argv, compiler, project_root, extra_include_dirs,
                                error, error_size) != 0 ||
            forge_argv_append(argv, "-c") != 0 ||
            forge_argv_append(argv, source_path) != 0 ||
            forge_argv_append(argv, "-o") != 0 ||
            forge_argv_append(argv, object_path) != 0) {
            forge_util_set_error(error, error_size, "out of memory while building compile command");
            forge_argv_free(argv);
            return -1;
        }
    } else {
        /*
         * -MMD -MF has GCC/Clang write the headers this translation unit
         * actually pulled in (system headers omitted) into a .d sibling of
         * the object; Forge reads it back so a header edit recompiles only
         * the files that use it.
         */
        program = language == FORGE_SOURCE_CPP
                      ? cpp_driver(compiler, cpp_program, sizeof(cpp_program))
                      : compiler->program;
        if (forge_compiler_depfile_path(object_path, depfile_path,
                                        sizeof(depfile_path)) != 0) {
            forge_util_set_error(error, error_size, "dependency file path is too long");
            return -1;
        }
        if (forge_argv_append(argv, program) != 0 ||
            append_include_dirs(argv, compiler, project_root, extra_include_dirs,
                                error, error_size) != 0 ||
            forge_argv_append(argv, "-MMD") != 0 ||
            forge_argv_append(argv, "-MF") != 0 ||
            forge_argv_append(argv, depfile_path) != 0 ||
            forge_argv_append(argv, "-c") != 0 ||
            forge_argv_append(argv, source_path) != 0 ||
            forge_argv_append(argv, "-o") != 0 ||
            forge_argv_append(argv, object_path) != 0) {
            forge_util_set_error(error, error_size, "out of memory while building compile command");
            forge_argv_free(argv);
            return -1;
        }
    }

    /*
     * The project version rides along as a preprocessor define so programs
     * can report themselves (`#define FORGE_PROJECT_VERSION "1.2.3"`). The
     * quotes are literal characters inside the argv element: on POSIX they
     * reach the compiler verbatim, and on Windows process.c escapes them for
     * the CreateProcess command line, so both dialects end up with a quoted
     * string macro. Assembly is left untouched (ml64 has no use for it).
     */
    if (language != FORGE_SOURCE_ASM && project_version != NULL &&
        project_version[0] != '\0') {
        char define[FORGE_MANIFEST_VALUE_MAX + 40U];
        const char *prefix = compiler->kind == FORGE_COMPILER_MSVC ? "/" : "-";

        if ((size_t)snprintf(define, sizeof(define),
                             "%sDFORGE_PROJECT_VERSION=\"%s\"",
                             prefix, project_version) >= sizeof(define)) {
            forge_util_set_error(error, error_size, "project version is too long");
            forge_argv_free(argv);
            return -1;
        }
        if (forge_argv_append(argv, define) != 0) {
            forge_util_set_error(error, error_size,
                                 "out of memory while building compile command");
            forge_argv_free(argv);
            return -1;
        }
    }

    if (forge_flags_append(argv, compiler, profile, 0, error, error_size) != 0) {
        forge_argv_free(argv);
        return -1;
    }
    return 0;
}

#define FORGE_LINK_RESPONSE_LIMIT 28000U

/* Writes the non-program tail of a link argv into a compiler response file,
 * using forward slashes so both GCC/Clang and MSVC parse the paths. */
static int write_response_file(const char *path, const ForgeArgv *argv,
                               char *error, size_t error_size)
{
    FILE *stream;
    size_t index;

    stream = fopen(path, "w");
    if (stream == NULL) {
        forge_util_set_error(error, error_size, "could not write response file '%s'", path);
        return -1;
    }
    for (index = 1U; index < argv->count; ++index) {
        const char *cursor;
        if (argv->items[index] == NULL) {
            break;
        }
        (void)fputc('"', stream);
        for (cursor = argv->items[index]; *cursor != '\0'; ++cursor) {
            if (*cursor == '"') {
                (void)fputc('\\', stream);
                (void)fputc('"', stream);
            } else if (*cursor == '\\') {
                (void)fputc('/', stream);
            } else {
                (void)fputc(*cursor, stream);
            }
        }
        (void)fputs("\"\n", stream);
    }
    if (fclose(stream) != 0) {
        forge_util_set_error(error, error_size, "could not finish response file '%s'", path);
        return -1;
    }
    return 0;
}

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
                                  char *error, size_t error_size)
{
    const char *program;
    char cpp_program[FORGE_COMPILER_VALUE_MAX];
    size_t index;
    ForgeArgv response = {0};
    char response_path[FORGE_PATH_MAX_LOCAL];

    if (compiler == NULL || object_paths == NULL || object_count == 0U ||
        output_path == NULL || output_path[0] == '\0' || response_dir == NULL ||
        profile == NULL || argv == NULL || used_response_file == NULL) {
        forge_util_set_error(error, error_size, "compiler, object files, output, profile, and argv output are required");
        return -1;
    }
    if (argv->count != 0U) {
        forge_util_set_error(error, error_size, "link argv output must be empty");
        return -1;
    }
    *used_response_file = 0;

    program = has_cpp_source ? cpp_driver(compiler, cpp_program, sizeof(cpp_program))
                             : compiler->program;
    if (compiler->kind == FORGE_COMPILER_MSVC) {
        program = compiler->program;
        if (forge_argv_append(argv, program) != 0 ||
            forge_argv_append(argv, "/nologo") != 0) {
            forge_util_set_error(error, error_size, "out of memory while building link command");
            return -1;
        }
    } else if (forge_argv_append(argv, program) != 0) {
        forge_util_set_error(error, error_size, "out of memory while building link command");
        return -1;
    }

    for (index = 0U; index < object_count; ++index) {
        if (object_paths[index] == NULL || object_paths[index][0] == '\0' ||
            forge_argv_append(argv, object_paths[index]) != 0) {
            forge_util_set_error(error, error_size, "invalid object file list");
            forge_argv_free(argv);
            return -1;
        }
    }

    /* Dependency objects and static libraries resolve symbols from the
     * project's own objects, so they come right after them. */
    for (index = 0U; index < extra_link_input_count; ++index) {
        if (extra_link_inputs[index] == NULL || extra_link_inputs[index][0] == '\0' ||
            forge_argv_append(argv, extra_link_inputs[index]) != 0) {
            forge_util_set_error(error, error_size, "invalid dependency link input list");
            forge_argv_free(argv);
            return -1;
        }
    }

    if (compiler->kind == FORGE_COMPILER_MSVC) {
        if (forge_argv_appendf(argv, "/Fe%s", output_path) != 0 ||
            forge_argv_append(argv, "/link") != 0) {
            forge_util_set_error(error, error_size, "out of memory while building link command");
            forge_argv_free(argv);
            return -1;
        }
    } else if (forge_argv_append(argv, "-o") != 0 ||
               forge_argv_append(argv, output_path) != 0) {
        forge_util_set_error(error, error_size, "out of memory while building link command");
        forge_argv_free(argv);
        return -1;
    }

    if (forge_flags_append(argv, compiler, profile, 1, error, error_size) != 0) {
        forge_argv_free(argv);
        return -1;
    }

    if (forge_argv_flatten_bytes(argv) <= FORGE_LINK_RESPONSE_LIMIT) {
        return 0;
    }
    if (snprintf(response_path, sizeof(response_path), "%s/link.rsp",
                 response_dir) < 0 ||
        strlen(response_dir) + 10U >= sizeof(response_path)) {
        forge_util_set_error(error, error_size, "response file path is too long");
        forge_argv_free(argv);
        return -1;
    }
    if (write_response_file(response_path, argv, error, error_size) != 0) {
        forge_argv_free(argv);
        return -1;
    }
    response = (ForgeArgv){0};
    if (forge_argv_append(&response, argv->items[0]) != 0 ||
        forge_argv_appendf(&response, "@%s", response_path) != 0) {
        forge_util_set_error(error, error_size, "out of memory while building response command");
        forge_argv_free(&response);
        forge_argv_free(argv);
        return -1;
    }
    forge_argv_free(argv);
    *argv = response;
    *used_response_file = 1;
    return 0;
}
