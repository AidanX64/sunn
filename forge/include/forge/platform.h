#ifndef FORGE_PLATFORM_H
#define FORGE_PLATFORM_H

/* MinGW/MSYS2 targets the Windows API even when _WIN32 is not defined. */
#if defined(_WIN32) || defined(__MINGW32__) || defined(__MINGW64__) || defined(__MSYS__)
#define FORGE_PLATFORM_WINDOWS 1
#else
#define FORGE_PLATFORM_WINDOWS 0
#endif

#endif
