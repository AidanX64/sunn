#ifndef FORGE_DEBUG_H
#define FORGE_DEBUG_H

#include <stddef.h>

#include "forge/log.h"

/*
 * Runs the host debugger, saves its raw transcript, and emits a concise
 * stack/disassembly-oriented view to the invocation log.
 */
int forge_debug_launch(const char *executable_path, ForgeLogger *logger,
                       char *error, size_t error_size);

#endif
