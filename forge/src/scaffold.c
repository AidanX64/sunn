#include <stdio.h>
#include <string.h>
#include <sys/stat.h>

#include "forge/manifest.h"
#include "forge/paths.h"
#include "forge/scaffold.h"
#include "forge_util.h"

#define FORGE_SCAFFOLD_LINE_MAX 512U

static const char *MANIFEST_TEMPLATE =
    "[project]\n"
    "name = \"%s\"\n"
    "version = \"0.1.0\"\n"
    "\n"
    "[sources]\n"
    "c = [\"src\"]\n"
    "cpp = []\n"
    "asm = []\n"
    "\n"
    "[targets]\n"
    "os = [\"windows\", \"linux\", \"macos\"]\n"
    "arch = [\"x86_64\", \"aarch64\"]\n"
    "\n"
    "[profile.debug]\n"
    "opt-level = 0\n"
    "debug = true\n"
    "\n"
    "[profile.release]\n"
    "opt-level = 2\n"
    "debug = false\n";

static const char *MAIN_TEMPLATE =
    "#include <stdio.h>\n"
    "\n"
    "int main(void)\n"
    "{\n"
    "    printf(\"Hello world!\\n\");\n"
    "    return 0;\n"
    "}\n";

static const char *GITIGNORE_TEMPLATE = "target/\n";

/* Project names must survive the manifest parser and become portable output
 * names, so restrict them to the same portable ASCII charset forge uses
 * everywhere else (explicit ranges rather than isalnum, which is
 * locale-dependent). */
static int name_is_valid(const char *name)
{
    size_t index;

    if (name == NULL || name[0] == '\0') {
        return 0;
    }
    for (index = 0U; name[index] != '\0'; ++index) {
        unsigned char character = (unsigned char)name[index];

        if (!((character >= 'a' && character <= 'z') ||
              (character >= 'A' && character <= 'Z') ||
              (character >= '0' && character <= '9') ||
              character == '-' || character == '_' || character == '.')) {
            return 0;
        }
    }
    return 1;
}

static int write_text_file(const char *path, const char *contents,
                           char *error, size_t error_size)
{
    FILE *file = fopen(path, "w");

    if (file == NULL) {
        forge_util_set_error(error, error_size, "could not create '%s'", path);
        return -1;
    }
    if (fputs(contents, file) < 0 || fclose(file) != 0) {
        forge_util_set_error(error, error_size, "could not write '%s'", path);
        return -1;
    }
    return 0;
}

/* Writes the scaffold files into `project_dir` (creating its src/ directory)
 * and verifies the generated manifest actually parses before reporting
 * success. When `refuse_existing` is set (forge init), any file that is
 * already there aborts the scaffold instead of being overwritten — a
 * hello-world must never replace someone's src/main.c. */
static int scaffold_into(const char *project_dir, const char *project_name,
                         int refuse_existing, char *error, size_t error_size)
{
    char manifest_path[FORGE_PATH_MAX];
    char main_path[FORGE_PATH_MAX];
    char gitignore_path[FORGE_PATH_MAX];
    char src_dir[FORGE_PATH_MAX];
    char manifest_contents[FORGE_SCAFFOLD_LINE_MAX];
    ForgeManifest parsed;
    int written;

    written = snprintf(manifest_contents, sizeof(manifest_contents),
                       MANIFEST_TEMPLATE, project_name);
    if (written < 0 || (size_t)written >= sizeof(manifest_contents)) {
        forge_util_set_error(error, error_size, "project name is too long");
        return -1;
    }
    if (forge_paths_join(src_dir, sizeof(src_dir), project_dir, "src") != 0 ||
        forge_paths_join(manifest_path, sizeof(manifest_path), project_dir,
                         "Forge.toml") != 0 ||
        forge_paths_join(main_path, sizeof(main_path), src_dir, "main.c") != 0 ||
        forge_paths_join(gitignore_path, sizeof(gitignore_path), project_dir,
                         ".gitignore") != 0) {
        forge_util_set_error(error, error_size, "project path is too long");
        return -1;
    }
    if (refuse_existing) {
        struct stat details;

        if (stat(main_path, &details) == 0) {
            forge_util_set_error(error, error_size,
                      "'src/main.c' already exists; forge init does not overwrite "
                      "existing files");
            return -1;
        }
        if (stat(gitignore_path, &details) == 0) {
            forge_util_set_error(error, error_size,
                      "'.gitignore' already exists; forge init does not overwrite "
                      "existing files");
            return -1;
        }
    }
    if (forge_paths_ensure_directory(src_dir, error, error_size) != 0) {
        return -1;
    }
    if (write_text_file(manifest_path, manifest_contents, error, error_size) != 0 ||
        write_text_file(main_path, MAIN_TEMPLATE, error, error_size) != 0 ||
        write_text_file(gitignore_path, GITIGNORE_TEMPLATE, error, error_size) != 0) {
        return -1;
    }
    /* The generated manifest must satisfy forge's own strict parser. */
    if (forge_manifest_load(manifest_path, &parsed, error, error_size) != 0) {
        forge_util_set_error(error, error_size,
                             "generated manifest failed validation: %s", error);
        return -1;
    }
    return 0;
}

int forge_scaffold_new_project(const char *name, char *error, size_t error_size)
{
    char project_dir[FORGE_PATH_MAX];
    struct stat details;

    if (!name_is_valid(name)) {
        forge_util_set_error(error, error_size,
                  "'%s' is not a valid project name; use letters, digits, '-', '_', '.'",
                  name == NULL ? "" : name);
        return -1;
    }
    if (forge_paths_join(project_dir, sizeof(project_dir), ".", name) != 0) {
        forge_util_set_error(error, error_size, "project path is too long");
        return -1;
    }
    if (stat(project_dir, &details) == 0) {
        forge_util_set_error(error, error_size,
                  "directory '%s' already exists; remove it or pick another name", name);
        return -1;
    }
    if (forge_paths_ensure_directory(project_dir, error, error_size) != 0) {
        return -1;
    }
    return scaffold_into(project_dir, name, 0, error, error_size);
}

int forge_scaffold_init_project(char *error, size_t error_size)
{
    ForgeManifest existing;
    char current[FORGE_PATH_MAX];
    const char *slash = NULL;
    const char *backslash = NULL;
    const char *base;

    if (forge_manifest_load("Forge.toml", &existing, error, error_size) == 0) {
        forge_util_set_error(error, error_size,
                             "this directory already has a valid Forge.toml");
        return -1;
    }
    /*
     * A missing manifest is expected here. A manifest that exists but fails
     * to parse still blocks init: overwriting a broken-but-precious file the
     * user meant to fix would destroy it.
     */
    {
        struct stat details;

        if (stat("Forge.toml", &details) == 0) {
            forge_util_set_error(error, error_size,
                      "'Forge.toml' exists but does not parse; fix or remove it "
                      "before running forge init");
            return -1;
        }
    }
    if (forge_paths_current_directory(current, sizeof(current)) != 0) {
        forge_util_set_error(error, error_size, "could not read the current directory");
        return -1;
    }
    slash = strrchr(current, '/');
    backslash = strrchr(current, '\\');
    base = slash != NULL && (backslash == NULL || slash > backslash) ? slash + 1 :
           backslash != NULL                                        ? backslash + 1 :
                                                                      current;
    if (!name_is_valid(base)) {
        /* The directory name is not a portable project name; fall back to a
         * neutral one the user can edit in Forge.toml. */
        base = "app";
    }
    return scaffold_into(".", base, 1, error, error_size);
}
