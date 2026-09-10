#ifndef FORGE_SCAFFOLD_H
#define FORGE_SCAFFOLD_H

#include <stddef.h>

/*
 * Creates a new project directory named `name` under the current working
 * directory containing Forge.toml, src/main.c, and .gitignore. Refuses to
 * touch an existing directory. Returns 0 on success.
 */
int forge_scaffold_new_project(const char *name, char *error, size_t error_size);

/* Scaffolds Forge.toml, src/main.c, and .gitignore into the current working
 * directory. Refuses when a manifest already exists there. Returns 0 on
 * success. */
int forge_scaffold_init_project(char *error, size_t error_size);

#endif
