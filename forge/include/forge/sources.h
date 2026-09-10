#ifndef FORGE_SOURCES_H
#define FORGE_SOURCES_H

#include <stddef.h>

#include "forge/compiler.h"
#include "forge/manifest.h"
#include "forge/paths.h"

/* Upper bound on sources in a single invocation. */
#define FORGE_MAX_SOURCE_FILES 4096U

typedef struct ForgeSourceFile {
    char path[FORGE_PATH_MAX];
    ForgeSourceLanguage language;
} ForgeSourceFile;

typedef struct ForgeSourceList {
    ForgeSourceFile *items;
    size_t count;
    size_t capacity;
} ForgeSourceList;

/*
 * Recursively collects C/C++/assembly sources from the manifest's source
 * directories. Returns 0 on success, -1 with a human-readable message in
 * `error` otherwise. `forge_sources_free` must be called for the result.
 */
int forge_sources_collect(const char *project_root, const ForgeManifest *manifest,
                          ForgeSourceList *sources, char *error, size_t error_size);

void forge_sources_free(ForgeSourceList *sources);

#endif