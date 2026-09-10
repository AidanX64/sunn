/*
 * Self-contained check that the manifest's [project] version reaches the
 * build as the FORGE_PROJECT_VERSION string macro. Kept separate from
 * src/main.c so `forge run` keeps printing exactly "Hello world!".
 */
#include <string.h>

int main(void)
{
#ifdef FORGE_PROJECT_VERSION
    if (strcmp(FORGE_PROJECT_VERSION, "0.1.0") == 0) {
        return 0;
    }
#endif
    return 1;
}
