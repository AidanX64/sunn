#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "forge/argv.h"

static int forge_argv_grow(ForgeArgv *argv)
{
    size_t new_capacity;
    char **expanded;

    if (argv->count < argv->capacity) {
        return 0;
    }
    new_capacity = argv->capacity == 0U ? 16U : argv->capacity * 2U;
    expanded = realloc(argv->items, new_capacity * sizeof(*argv->items));
    if (expanded == NULL) {
        return -1;
    }
    argv->items = expanded;
    argv->capacity = new_capacity;
    return 0;
}

int forge_argv_append(ForgeArgv *argv, const char *text)
{
    char *copy;

    if (argv == NULL || text == NULL) {
        return -1;
    }
    if (forge_argv_grow(argv) != 0) {
        return -1;
    }
    copy = malloc(strlen(text) + 1U);
    if (copy == NULL) {
        return -1;
    }
    (void)strcpy(copy, text);
    argv->items[argv->count] = copy;
    ++argv->count;
    return 0;
}

int forge_argv_appendf(ForgeArgv *argv, const char *format, ...)
{
    va_list arguments;
    char buffer[4096U];
    int written;

    va_start(arguments, format);
    written = vsnprintf(buffer, sizeof(buffer), format, arguments);
    va_end(arguments);
    if (written < 0 || (size_t)written >= sizeof(buffer)) {
        return -1;
    }
    return forge_argv_append(argv, buffer);
}

int forge_argv_finalize(ForgeArgv *argv)
{
    if (argv == NULL) {
        return -1;
    }
    if (forge_argv_grow(argv) != 0) {
        return -1;
    }
    argv->items[argv->count] = NULL;
    return 0;
}

void forge_argv_free(ForgeArgv *argv)
{
    size_t index;

    if (argv == NULL) {
        return;
    }
    for (index = 0U; index < argv->count; ++index) {
        free(argv->items[index]);
    }
    free(argv->items);
    *argv = (ForgeArgv){0};
}

size_t forge_argv_flatten_bytes(const ForgeArgv *argv)
{
    size_t total = 0U;
    size_t index;

    if (argv == NULL) {
        return 0U;
    }
    for (index = 0U; index < argv->count; ++index) {
        if (argv->items[index] != NULL) {
            total += strlen(argv->items[index]) + 1U;
        }
    }
    return total;
}

int forge_argv_join(char *destination, size_t destination_size, const ForgeArgv *argv)
{
    size_t length = 0U;
    size_t index;

    if (destination == NULL || destination_size == 0U || argv == NULL) {
        return -1;
    }
    destination[0] = '\0';
    for (index = 0U; index < argv->count; ++index) {
        if (argv->items[index] == NULL) {
            break;
        }
        if (index != 0U) {
            if (length + 1U >= destination_size) {
                return -1;
            }
            destination[length++] = ' ';
        }
        if (snprintf(destination + length, destination_size - length, "%s",
                     argv->items[index]) < 0) {
            return -1;
        }
        length = strlen(destination);
    }
    return 0;
}