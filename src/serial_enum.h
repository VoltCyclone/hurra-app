/*
 * serial_enum.h — enumerate serial/COM ports and classify them.
 *
 * The classifier (serial_classify_hwid) is pure and always compiled, so it can
 * be unit-tested on any host. The enumerator (serial_enum) is Windows-only in
 * practice; a stub returns 0 on other platforms.
 */
#ifndef HURRA_SERIAL_ENUM_H
#define HURRA_SERIAL_ENUM_H

#include <stddef.h>

typedef enum {
    PORT_OTHER = 0,    /* unknown / unrelated COM port            */
    PORT_FIRMWARE,     /* WCH CH343 (VID_1A86) — the Hurra device */
    PORT_COM0COM       /* com0com virtual pair end                */
} port_class_t;

typedef struct {
    char         name[32];      /* "COM5"                    */
    char         friendly[128]; /* "USB-SERIAL CH343 (COM5)" */
    port_class_t klass;
} serial_cand_t;

/* Classify a Win32 hardware-ID string. Pure; NULL or "" -> PORT_OTHER.
 * Match is case-insensitive substring:
 *   "VID_1A86"                  -> PORT_FIRMWARE
 *   "COM0COM" | "CNCA" | "CNCB" -> PORT_COM0COM
 *   otherwise                   -> PORT_OTHER
 */
port_class_t serial_classify_hwid(const char *hwid);

/* Enumerate present COM ports, newest API available per platform. Fills up to
 * `max` candidates, returns the count. Returns 0 on failure or non-Windows. */
size_t serial_enum(serial_cand_t *out, size_t max);

#endif /* HURRA_SERIAL_ENUM_H */
