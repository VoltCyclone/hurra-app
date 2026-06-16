/* serial_enum.c — pure hardware-ID classifier (always compiled). */
#include "serial_enum.h"
#include <ctype.h>

/* Case-insensitive substring search. Returns 1 if `needle` is in `hay`. */
static int ci_contains(const char *hay, const char *needle) {
    if (!hay || !needle || !*needle) return 0;
    for (; *hay; hay++) {
        const char *h = hay, *n = needle;
        while (*h && *n &&
               tolower((unsigned char)*h) == tolower((unsigned char)*n)) {
            h++; n++;
        }
        if (!*n) return 1;
    }
    return 0;
}

port_class_t serial_classify_hwid(const char *hwid) {
    if (!hwid || !*hwid) return PORT_OTHER;
    if (ci_contains(hwid, "VID_1A86")) return PORT_FIRMWARE;
    if (ci_contains(hwid, "COM0COM") ||
        ci_contains(hwid, "CNCA")    ||
        ci_contains(hwid, "CNCB"))    return PORT_COM0COM;
    return PORT_OTHER;
}
