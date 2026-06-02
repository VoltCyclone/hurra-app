/*
 * ui_util.h — pure, side-effect-free helpers for hurra-bridge's terminal UI.
 *
 * Header-only (static inline) so the single bridge translation unit can use
 * them and an ad-hoc test can include them directly. Nothing here does I/O,
 * touches globals, or depends on a TTY — keep it that way so it stays testable.
 */
#ifndef HURRA_UI_UTIL_H
#define HURRA_UI_UTIL_H

#include <errno.h>
#include <stdint.h>
#include <stdio.h>

/* "4000000" -> "4 Mbaud"; exact-million only. Otherwise "<n> baud".
 * Returns buf. */
static inline char *ui_humanize_baud(uint32_t baud, char *buf, size_t n) {
    if (baud >= 1000000u && baud % 1000000u == 0u)
        snprintf(buf, n, "%u Mbaud", (unsigned)(baud / 1000000u));
    else
        snprintf(buf, n, "%u baud", (unsigned)baud);
    return buf;
}

/* Seconds -> compact "45s" / "1m24s" / "1h01m01s". Returns buf. */
static inline char *ui_humanize_uptime(uint64_t secs, char *buf, size_t n) {
    uint64_t h = secs / 3600u;
    uint64_t m = (secs % 3600u) / 60u;
    uint64_t s = secs % 60u;
    if (h)      snprintf(buf, n, "%lluh%02llum%02llus",
                         (unsigned long long)h, (unsigned long long)m,
                         (unsigned long long)s);
    else if (m) snprintf(buf, n, "%llum%02llus",
                         (unsigned long long)m, (unsigned long long)s);
    else        snprintf(buf, n, "%llus", (unsigned long long)s);
    return buf;
}

/* Integer with thousands separators: 12480 -> "12,480". Returns buf. */
static inline char *ui_group_thousands(uint64_t v, char *buf, size_t n) {
    char tmp[32];
    int len = snprintf(tmp, sizeof tmp, "%llu", (unsigned long long)v);
    int out = 0, until_sep = len % 3 == 0 ? 3 : len % 3;
    for (int i = 0; i < len && out < (int)n - 1; i++) {
        if (i && until_sep == 0) {
            buf[out++] = ',';
            until_sep = 3;
            if (out >= (int)n - 1) { out--; break; }  /* no room for next digit; drop trailing comma */
        }
        buf[out++] = tmp[i];
        until_sep--;
    }
    buf[out] = '\0';
    return buf;
}

/* Open-failure categories for friendly device-open diagnostics. */
typedef enum {
    UI_OPEN_NOENT,   /* no such device */
    UI_OPEN_ACCES,   /* permission denied */
    UI_OPEN_BUSY,    /* device in use */
    UI_OPEN_OTHER    /* fall back to strerror */
} ui_open_cat_t;

static inline ui_open_cat_t ui_open_category(int err) {
    switch (err) {
        case ENOENT:
#ifdef ENXIO
        case ENXIO:
#endif
#ifdef ENODEV
        case ENODEV:
#endif
            return UI_OPEN_NOENT;
        case EACCES:
#ifdef EPERM
        case EPERM:
#endif
            return UI_OPEN_ACCES;
        case EBUSY:
            return UI_OPEN_BUSY;
        default:
            return UI_OPEN_OTHER;
    }
}

/* Link-health label from cumulative probe counters (mirrors heartbeat logic). */
static inline const char *ui_link_health(uint32_t ok, uint32_t fail) {
    if (fail == 0 && ok > 0) return "ok";
    if (ok == 0 && fail > 0) return "dead";
    if (ok > 0 && fail > 0)  return "flapping";
    return "unknown";
}

/* ASCII spinner frame for index i (wraps). */
static inline char ui_spinner_ascii(uint64_t i) {
    static const char frames[] = { '|', '/', '-', '\\' };
    return frames[i % 4];
}

/* Unicode (Braille) spinner frame for index i (wraps). Returns a NUL-terminated
 * multibyte string literal; used only when the terminal supports UTF-8/VT. */
static inline const char *ui_spinner_braille(uint64_t i) {
    static const char *frames[] = {
        "\xe2\xa0\x8b","\xe2\xa0\x99","\xe2\xa0\xb9","\xe2\xa0\xb8",
        "\xe2\xa0\xbc","\xe2\xa0\xb4","\xe2\xa0\xa6","\xe2\xa0\xa7",
        "\xe2\xa0\x87","\xe2\xa0\x8f"
    };
    return frames[i % 10];
}

#endif /* HURRA_UI_UTIL_H */
