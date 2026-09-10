#ifndef FORGE_FLAGS_H
#define FORGE_FLAGS_H

#include <stddef.h>

#include "forge/argv.h"
#include "forge/compiler.h"
#include "forge/manifest.h"

/*
 * Appends the complete flag set for `profile` to `argv`, rendered into the
 * dialect of `compiler`: the structured fields (opt-level, debug, warnings,
 * std) are translated per compiler kind, and raw `cflags` are passed through
 * for GCC/Clang or translated (or rejected with a clear diagnostic) for MSVC.
 * `for_link` adjusts stage-specific flags such as MSVC /DEBUG.
 */
int forge_flags_append(ForgeArgv *argv, const ForgeCompiler *compiler,
                       const ForgeBuildProfile *profile, int for_link,
                       char *error, size_t error_size);

#endif