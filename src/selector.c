#include "selector.h"
#include <stdio.h>
#ifdef _WIN32
#  include <io.h>
#  define ISATTY(fd) _isatty(fd)
#  define FILENO _fileno
#else
#  include <unistd.h>
#  define ISATTY(fd) isatty(fd)
#  define FILENO fileno
#endif

int selector_choose(void) {
    if (!ISATTY(FILENO(stdin))) return -1;
    fprintf(stderr,
        "\nhurra-bridge\n\n"
        "  Select an endpoint:\n"
        "    1) Virtual COM port (Ferrum-compatible)   [default]\n"
        "    2) KMBox Net (UDP)\n\n  > ");
    fflush(stderr);
    int c = fgetc(stdin);
    int x = c; while (x != '\n' && x != EOF) x = fgetc(stdin);  /* drain line */
    if (c == '2') return ENDPOINT_KMBOX;
    return ENDPOINT_VCOM;  /* '1', Enter, or anything else -> default */
}
