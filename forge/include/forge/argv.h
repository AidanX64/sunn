#ifndef FORGE_ARGV_H
#define FORGE_ARGV_H

#include <stddef.h>

/*
 * A growable array of argument strings for spawning a child process without a
 * shell. Each appended string is copied. Call forge_argv_finalize before use
 * to close the list with a NULL terminator, then free with forge_argv_free.
 */
typedef struct ForgeArgv {
    char **items;
    size_t count;
    size_t capacity;
} ForgeArgv;

int forge_argv_append(ForgeArgv *argv, const char *text);
int forge_argv_appendf(ForgeArgv *argv, const char *format, ...);
int forge_argv_finalize(ForgeArgv *argv);
void forge_argv_free(ForgeArgv *argv);

/* Bytes needed to join every argument (minus the NULL terminator), used to
 * decide whether a response file is required for a compiler invocation. */
size_t forge_argv_flatten_bytes(const ForgeArgv *argv);

/* Joins the arguments with a single space (display/logging purposes). */
int forge_argv_join(char *destination, size_t destination_size, const ForgeArgv *argv);

#endif