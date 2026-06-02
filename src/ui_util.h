/*
 * ui_util.h — pure, side-effect-free helpers for hurra-bridge's terminal UI.
 *
 * Header-only (static inline) so the single bridge translation unit can use
 * them and an ad-hoc test can include them directly. Nothing here does I/O,
 * touches globals, or depends on a TTY — keep it that way so it stays testable.
 */
#ifndef HURRA_UI_UTIL_H
#define HURRA_UI_UTIL_H

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
    int out = 0, since = len % 3 == 0 ? 3 : len % 3;
    for (int i = 0; i < len && out < (int)n - 1; i++) {
        if (i && since == 0) { buf[out++] = ','; since = 3; if (out >= (int)n - 1) break; }
        buf[out++] = tmp[i];
        since--;
    }
    buf[out] = '\0';
    return buf;
}

#endif /* HURRA_UI_UTIL_H */
