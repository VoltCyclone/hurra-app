/*
 * bridge.c — hurra-bridge: ferrum-on-PTY ↔ hurra-on-serial bridge.
 *
 * Reads Ferrum ASCII commands from a virtual COM port (PTY on Unix,
 * com0com on Windows), translates them to Hurra binary frames over a real
 * serial link to the firmware. Telemetry from the firmware is translated
 * back into Ferrum-compatible text on the virtual port.
 *
 * Single-threaded; the main loop pumps both transports with a short
 * sleep between iterations. The bridge is not latency-critical at the
 * millisecond scale — the firmware does the real work.
 */

#include "ferrum_parser.h"
#include "hurra.h"
#include "hurra_types.h"
#include "virtual_port.h"
#include "ui_util.h"

#include <errno.h>
#include <signal.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef _WIN32
#  define WIN32_LEAN_AND_MEAN
#  include <windows.h>
#  include <io.h>          /* _isatty, _fileno */
#  define HOME_ENV "USERPROFILE"
#else
#  include <unistd.h>
#  include <time.h>
#  include <glob.h>
#  include <fcntl.h>       /* open, O_RDWR for diag_open_errno */
#  define HOME_ENV "HOME"
#endif

/* ── Globals (this is a single-process daemon; globals are fine) ────────── */

static volatile sig_atomic_t g_stop = 0;

static void on_signal(int sig) {
    (void)sig;
    g_stop = 1;
}

/* ── Clock ──────────────────────────────────────────────────────────────── */

static uint64_t mono_ms(void) {
#ifdef _WIN32
    LARGE_INTEGER f, c;
    QueryPerformanceFrequency(&f);
    QueryPerformanceCounter(&c);
    return (uint64_t)((c.QuadPart * 1000ULL) / f.QuadPart);
#else
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000ULL + (uint64_t)(ts.tv_nsec / 1000000L);
#endif
}

/* ── Bridge state ───────────────────────────────────────────────────────── */

typedef struct {
    hurra_client_t  *hc;
    vp_port_t       *vp;
    ferrum_parser_t *parser;

    /* Per-callback local enable state (ferrum semantics: readable). */
    bool cb_buttons_enabled;
    bool cb_axes_enabled;
    bool cb_keys_enabled;

    int  request_timeout_ms;

    /* Diagnostics — bumped at hot-path points, dumped by the 5s heartbeat
     * and by the __diag__ side-channel. Single-threaded process; no
     * synchronization needed. uint64_t to handle long-running daemons. */
    uint64_t hurra_tx_bytes;
    uint64_t hurra_tx_frames;
    uint64_t hurra_rx_bytes;
    uint64_t ferrum_lines_in;       /* lines successfully dispatched from PTY */
    uint64_t ferrum_moves;          /* km.move() count */
    uint32_t probe_calls;
    uint32_t probe_ok;
    uint32_t probe_fail;
    uint64_t start_ms;              /* monotonic ms at bridge start */
} bridge_t;

/* ── PTY write helpers ──────────────────────────────────────────────────── */

static void vp_write_all(vp_port_t *vp, const uint8_t *buf, size_t n) {
    size_t off = 0;
    while (off < n) {
        int w = vp_write(vp, buf + off, n - off);
        if (w < 0) return;
        if (w == 0) {
            /* Buffer full / no reader. Drop the rest rather than spin
             * forever — a disconnected client should not block the bridge. */
            return;
        }
        off += (size_t)w;
    }
}

/* Writer adapter for the ferrum_emit_* helpers. */
static void writer_for_emit(const uint8_t *buf, size_t n, void *user) {
    bridge_t *b = (bridge_t *)user;
    vp_write_all(b->vp, buf, n);
}

/* ── Logging ────────────────────────────────────────────────────────────── */

static void blog(const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    vfprintf(stderr, fmt, ap);
    va_end(ap);
    fprintf(stderr, "\n");
    fflush(stderr);
}

/* ── Terminal UI layer ──────────────────────────────────────────────────────
 * Capability is detected once in ui_init(); every decoration checks g_ui. */

static struct {
    bool color;       /* color/VT escapes allowed */
    bool status_tty;  /* stdout is a live terminal (in-place status line ok) */
    bool utf8;        /* safe to emit the Braille spinner / box glyphs */
} g_ui;

#ifdef _WIN32
static int ui_isatty_fd(FILE *f) { return _isatty(_fileno(f)); }
#else
static int ui_isatty_fd(FILE *f) { return isatty(fileno(f)); }
#endif

/* Resolve color/status/utf8 from args + environment + TTY state. */
static void ui_init(bool no_color_flag) {
    bool stderr_tty = ui_isatty_fd(stderr) != 0;
    bool stdout_tty = ui_isatty_fd(stdout) != 0;
    const char *nc  = getenv("NO_COLOR");
    bool no_color   = no_color_flag || (nc && *nc);

    g_ui.color      = stderr_tty && !no_color;
    g_ui.status_tty = stdout_tty;

#ifdef _WIN32
    g_ui.utf8 = false; /* keep ASCII spinner/markers on Windows consoles */
    if (g_ui.color) {
        HANDLE h = GetStdHandle(STD_ERROR_HANDLE);
        DWORD mode = 0;
        if (h == INVALID_HANDLE_VALUE || !GetConsoleMode(h, &mode) ||
            !SetConsoleMode(h, mode | 0x0004 /*ENABLE_VIRTUAL_TERMINAL_PROCESSING*/)) {
            g_ui.color = false; /* terminal can't do VT; stay plain */
        }
    }
#else
    {
        /* Trust UTF-8 when color is on and locale looks UTF-8-ish. */
        const char *lc = getenv("LC_ALL");
        if (!lc || !*lc) lc = getenv("LC_CTYPE");
        if (!lc || !*lc) lc = getenv("LANG");
        g_ui.utf8 = g_ui.color && lc &&
                    (strstr(lc, "UTF-8") || strstr(lc, "UTF8") ||
                     strstr(lc, "utf-8") || strstr(lc, "utf8"));
    }
#endif
}

/* Color wrappers: return the escape only when color is enabled, else "". */
static const char *c_red(void)   { return g_ui.color ? "\x1b[31m" : ""; }
static const char *c_grn(void)   { return g_ui.color ? "\x1b[32m" : ""; }
static const char *c_yel(void)   { return g_ui.color ? "\x1b[33m" : ""; }
static const char *c_dim(void)   { return g_ui.color ? "\x1b[2m"  : ""; }
static const char *c_rst(void)   { return g_ui.color ? "\x1b[0m"  : ""; }

/* Status glyphs degrade to ASCII when utf8 is off. */
static const char *g_ok(void)   { return g_ui.utf8 ? "\xe2\x9c\x93" : "*"; } /* check */
static const char *g_bad(void)  { return g_ui.utf8 ? "\xe2\x9c\x97" : "x"; } /* cross */

/* Print a friendly, optionally-colored fatal error and return an exit code.
 * Format: "<x> <headline>\n  <cause/help lines...>". `body` may be multi-line. */
static int bridge_fail(int code, const char *headline, const char *body) {
    fprintf(stderr, "%s%s %s%s\n", c_red(), g_bad(), headline, c_rst());
    if (body && *body) fprintf(stderr, "%s%s%s\n", c_dim(), body, c_rst());
    fflush(stderr);
    return code;
}

#ifndef _WIN32
/* Re-open the device purely to capture a reliable errno (the real open path
 * frees/sleeps in between, clobbering errno before we can read it). Returns
 * the errno from a fresh open; 0 on success. */
static int diag_open_errno(const char *path) {
    errno = 0;
    int fd = open(path, O_RDWR | O_NOCTTY | O_NONBLOCK);
    if (fd >= 0) { close(fd); return 0; }
    return errno;
}
#endif

/* Print the aligned startup banner head (TTY) or a plain line (non-TTY). */
static void print_banner_head(void) {
    if (g_ui.status_tty) fprintf(stderr, "\nhurra-bridge\n\n");
    else                 fprintf(stderr, "hurra-bridge: starting\n");
}

/* ── Hurra telemetry → Ferrum text ──────────────────────────────────────── */

static void on_tlm_buttons(uint8_t type, const uint8_t *data,
                           uint16_t len, void *user) {
    (void)type;
    bridge_t *b = (bridge_t *)user;
    if (!b->cb_buttons_enabled || len < 1) return;
    ferrum_emit_buttons_cb(writer_for_emit, b, data[0]);
}

static void on_tlm_axis(uint8_t type, const uint8_t *data,
                        uint16_t len, void *user) {
    (void)type;
    bridge_t *b = (bridge_t *)user;
    if (!b->cb_axes_enabled || len < 5) return;
    /* TLM_AXIS payload: int16 dx | int16 dy | int8 scroll, little-endian. */
    int16_t dx = (int16_t)((uint16_t)data[0] | ((uint16_t)data[1] << 8));
    int16_t dy = (int16_t)((uint16_t)data[2] | ((uint16_t)data[3] << 8));
    int8_t  sc = (int8_t)data[4];
    ferrum_emit_axes_cb(writer_for_emit, b, dx, dy, sc);
}

static void on_tlm_kb(uint8_t type, const uint8_t *data,
                      uint16_t len, void *user) {
    (void)type;
    bridge_t *b = (bridge_t *)user;
    if (!b->cb_keys_enabled) return;
    uint8_t keys[6] = {0};
    size_t n = len < 6 ? len : 6;
    memcpy(keys, data, n);
    ferrum_emit_keys_cb(writer_for_emit, b, keys);
}

/* ── Ferrum callbacks → Hurra calls ─────────────────────────────────────── */

/* Map ferrum button-mask bit → hurra button index (0=L,1=R,2=M,3=S1,4=S2). */
static int btn_index_for_mask(uint8_t mask) {
    switch (mask) {
        case 0x01: return 0;
        case 0x02: return 1;
        case 0x04: return 2;
        case 0x08: return 3;
        case 0x10: return 4;
        default:   return -1;
    }
}

static void cb_version(void *user) {
    bridge_t *b = (bridge_t *)user;
    /* Always emit the canonical "kmbox: Ferrum\r\n" reply for compatibility.
     * Firmware link health is visible via stderr log and __diag__ side-channel. */
    char tmp[64] = {0};
    int rc = hurra_version(b->hc, tmp, sizeof(tmp), 250);
    b->probe_calls++;
    if (rc == 0) {
        b->probe_ok++;
        blog("version probe ok: fw=\"%s\"", tmp);
    } else {
        b->probe_fail++;
        blog("version probe FAILED: rc=%d  (firmware not responding on real UART)", rc);
    }
    ferrum_emit_version_text(writer_for_emit, b);
}

static void cb_move(int32_t x, int32_t y, void *user) {
    bridge_t *b = (bridge_t *)user;
    b->ferrum_moves++;
    if (x > INT16_MAX) x = INT16_MAX;
    if (x < INT16_MIN) x = INT16_MIN;
    if (y > INT16_MAX) y = INT16_MAX;
    if (y < INT16_MIN) y = INT16_MIN;
    int rc = hurra_move(b->hc, (int16_t)x, (int16_t)y);
    if (rc != 0) blog("hurra_move rc=%d", rc);
}

static void cb_button_set(uint8_t mask, uint8_t state, void *user) {
    bridge_t *b = (bridge_t *)user;
    int idx = btn_index_for_mask(mask);
    if (idx < 0) return;
    (void)hurra_button(b->hc, (uint8_t)idx, state);
}

static void cb_button_get(uint8_t mask, void *user) {
    bridge_t *b = (bridge_t *)user;
    int idx = btn_index_for_mask(mask);
    if (idx < 0) { ferrum_emit_bool(writer_for_emit, b, 0); return; }
    bool down = false;
    int rc = hurra_button_get(b->hc, (uint8_t)idx, &down, b->request_timeout_ms);
    ferrum_emit_bool(writer_for_emit, b, (rc == 0 && down) ? 1 : 0);
}

static void cb_click(uint8_t button_0based, void *user) {
    bridge_t *b = (bridge_t *)user;
    /* hurra_click takes 1-based button per spec. */
    (void)hurra_click(b->hc, (uint8_t)(button_0based + 1), 1, 0);
}

static void cb_wheel(int32_t n, void *user) {
    bridge_t *b = (bridge_t *)user;
    if (n > INT8_MAX) n = INT8_MAX;
    if (n < INT8_MIN) n = INT8_MIN;
    (void)hurra_wheel(b->hc, (int8_t)n);
}

static void cb_lock_set(const char *name, uint8_t state, void *user) {
    bridge_t *b = (bridge_t *)user;
    int s = state ? 1 : 0;
    (void)hurra_lock(b->hc, name, &s, b->request_timeout_ms);
}

static void cb_lock_get(const char *name, void *user) {
    bridge_t *b = (bridge_t *)user;
    int s = -1;
    int rc = hurra_lock(b->hc, name, &s, b->request_timeout_ms);
    ferrum_emit_bool(writer_for_emit, b, (rc == 0 && s) ? 1 : 0);
}

static void cb_catch_xy(uint32_t dur_ms, void *user) {
    bridge_t *b = (bridge_t *)user;
    if (dur_ms > 1000) dur_ms = 1000;
    int32_t dx = 0, dy = 0;
    int rc = hurra_catch_xy(b->hc, (uint16_t)dur_ms, &dx, &dy);
    if (rc != 0) { dx = 0; dy = 0; }
    ferrum_emit_pair(writer_for_emit, b, dx, dy);
}

static void cb_kb_down (uint8_t hid, void *user) {
    bridge_t *b = (bridge_t *)user; (void)hurra_kb_down (b->hc, hid);
}
static void cb_kb_up   (uint8_t hid, void *user) {
    bridge_t *b = (bridge_t *)user; (void)hurra_kb_up   (b->hc, hid);
}
static void cb_kb_press(uint8_t hid, void *user) {
    bridge_t *b = (bridge_t *)user;
    (void)hurra_kb_press(b->hc, hid, 80, 30);
}

static void cb_kb_multi(int op, const uint8_t *keys, size_t n, void *user) {
    bridge_t *b = (bridge_t *)user;
    switch (op) {
        case 0: (void)hurra_kb_multidown (b->hc, keys, n); break;
        case 1: (void)hurra_kb_multiup   (b->hc, keys, n); break;
        case 2: (void)hurra_kb_multipress(b->hc, keys, n); break;
    }
}

static void cb_kb_isdown(uint8_t hid, void *user) {
    bridge_t *b = (bridge_t *)user;
    bool out = false;
    int rc = hurra_kb_isdown(b->hc, hid, &out, b->request_timeout_ms);
    ferrum_emit_bool(writer_for_emit, b, (rc == 0 && out) ? 1 : 0);
}

static void cb_kb_mask_set(uint8_t hid, uint8_t state, void *user) {
    bridge_t *b = (bridge_t *)user;
    (void)hurra_kb_mask(b->hc, hid, state);
}

static void cb_kb_mask_get(uint8_t hid, void *user) {
    /* Ferrum returns 0 for the read path (no real getter). */
    (void)hid;
    bridge_t *b = (bridge_t *)user;
    ferrum_emit_bool(writer_for_emit, b, 0);
}

static void cb_init(void *user) {
    bridge_t *b = (bridge_t *)user;
    (void)hurra_init_remote(b->hc);
}

static void cb_baud(uint32_t baud, void *user) {
    bridge_t *b = (bridge_t *)user;
    /* Tells firmware to switch baud; bridge serial link stays at current rate. */
    (void)hurra_set_baud(b->hc, baud, b->request_timeout_ms);
}

static void cb_human(uint32_t level, void *user) {
    bridge_t *b = (bridge_t *)user;
    (void)hurra_human(b->hc, (uint8_t)level);
}

/* CB toggles: track local enable state and propagate to firmware. */
static void cb_cb_buttons_set(uint8_t enable, void *user) {
    bridge_t *b = (bridge_t *)user;
    b->cb_buttons_enabled = enable != 0;
    (void)hurra_cb_buttons(b->hc, b->cb_buttons_enabled ? 1 : 0);
}
static void cb_cb_buttons_get(void *user) {
    bridge_t *b = (bridge_t *)user;
    ferrum_emit_bool(writer_for_emit, b, b->cb_buttons_enabled ? 1 : 0);
}
static void cb_cb_axes_set(uint8_t enable, void *user) {
    bridge_t *b = (bridge_t *)user;
    b->cb_axes_enabled = enable != 0;
    (void)hurra_cb_axes(b->hc, b->cb_axes_enabled ? 1 : 0);
}
static void cb_cb_axes_get(void *user) {
    bridge_t *b = (bridge_t *)user;
    ferrum_emit_bool(writer_for_emit, b, b->cb_axes_enabled ? 1 : 0);
}
static void cb_cb_keys_set(uint8_t enable, void *user) {
    bridge_t *b = (bridge_t *)user;
    b->cb_keys_enabled = enable != 0;
    (void)hurra_cb_keys(b->hc, b->cb_keys_enabled ? 1 : 0);
}
static void cb_cb_keys_get(void *user) {
    bridge_t *b = (bridge_t *)user;
    ferrum_emit_bool(writer_for_emit, b, b->cb_keys_enabled ? 1 : 0);
}

/* ── Arg parsing ────────────────────────────────────────────────────────── */

static void usage(const char *prog) {
    fprintf(stderr,
        "hurra-bridge — Ferrum-compatible bridge to Hurra firmware\n"
        "\n"
        "usage: %s [--device PATH] [--baud N] [--link PATH]\n"
        "                   [--virtual-port NAME] [--timeout-ms N] [--no-color]\n"
        "\n"
        "  --device PATH        Real serial device (e.g. /dev/cu.usbmodem01, COM5).\n"
#ifdef _WIN32
        "                       Required on Windows.\n"
#else
        "                       Optional: auto-detected when exactly one port is found.\n"
#endif
        "  --baud N             Real-link baud rate. Default 4000000 (4 Mbaud).\n"
        "  --link PATH          Symlink to the PTY slave (Unix). Default $%s/.hurra-bridge.tty.\n"
        "  --virtual-port NAME  com0com COM name to open (Windows; required there).\n"
        "  --timeout-ms N       Per-request timeout for get-style commands. Default 250.\n"
        "  --no-color           Disable colored output (also honors NO_COLOR env).\n"
        "  -h, --help           Show this help and exit.\n",
        prog, HOME_ENV);
}

typedef struct {
    const char *device;
    uint32_t    baud;
    const char *link_path;       /* Unix only */
    const char *virtual_port;    /* Windows only */
    int         timeout_ms;
    bool        no_color;        /* --no-color: force-disable color */
} args_t;

static int parse_args(int argc, char **argv, args_t *out) {
    out->device = NULL;
    out->baud = 4000000;
    out->link_path = NULL;
    out->virtual_port = NULL;
    out->timeout_ms = 250;
    out->no_color = false;

    for (int i = 1; i < argc; i++) {
        const char *a = argv[i];
        if (!strcmp(a, "--device") && i + 1 < argc)       out->device = argv[++i];
        else if (!strcmp(a, "--baud") && i + 1 < argc)    out->baud = (uint32_t)strtoul(argv[++i], NULL, 10);
        else if (!strcmp(a, "--link") && i + 1 < argc)    out->link_path = argv[++i];
        else if (!strcmp(a, "--virtual-port") && i + 1 < argc) out->virtual_port = argv[++i];
        else if (!strcmp(a, "--timeout-ms") && i + 1 < argc)   out->timeout_ms = atoi(argv[++i]);
        else if (!strcmp(a, "--no-color"))                out->no_color = true;
        else if (!strcmp(a, "-h") || !strcmp(a, "--help")) { usage(argv[0]); return -1; }
        else { fprintf(stderr, "%s%s unknown option: %s%s\n",
                       c_red(), g_bad(), a, c_rst()); usage(argv[0]); return -1; }
    }
#ifdef _WIN32
    if (!out->device) {
        fprintf(stderr, "%s%s --device is required on Windows%s\n",
                c_red(), g_bad(), c_rst());
        usage(argv[0]);
        return -1;
    }
#endif
    /* On Unix, a missing --device triggers auto-discovery in main() (Task 7). */
    return 0;
}

/* Default Unix symlink path: $HOME/.hurra-bridge.tty. */
static char *default_link_path(void) {
    const char *home = getenv(HOME_ENV);
    if (!home || !*home) return NULL;
    size_t n = strlen(home) + sizeof("/.hurra-bridge.tty");
    char *out = (char *)malloc(n);
    if (!out) return NULL;
    snprintf(out, n, "%s/.hurra-bridge.tty", home);
    return out;
}

#ifndef _WIN32
/* Discovered serial-port candidate. */
typedef struct { char path[256]; bool wch; } dev_cand_t;

/* Enumerate likely serial ports, WCH/USB-serial first. Returns count, fills
 * up to `max` candidates. Unix only. */
static size_t discover_devices(dev_cand_t *out, size_t max) {
    static const char *globs[] = {
#if defined(__APPLE__)
        "/dev/cu.wchusbserial*", "/dev/cu.usbmodem*", "/dev/cu.usbserial*",
#else
        "/dev/ttyACM*", "/dev/ttyUSB*",
#endif
    };
    size_t n = 0;
    for (size_t gi = 0; gi < sizeof(globs)/sizeof(globs[0]) && n < max; gi++) {
        glob_t gl = {0};
        if (glob(globs[gi], 0, NULL, &gl) == 0) {
            for (size_t i = 0; i < gl.gl_pathc && n < max; i++) {
                /* Skip duplicates already collected. */
                bool dup = false;
                for (size_t k = 0; k < n; k++)
                    if (strcmp(out[k].path, gl.gl_pathv[i]) == 0) { dup = true; break; }
                if (dup) continue;
                snprintf(out[n].path, sizeof(out[n].path), "%s", gl.gl_pathv[i]);
                out[n].wch = strstr(gl.gl_pathv[i], "wch") != NULL;
                n++;
            }
        }
        globfree(&gl);
    }
    return n;
}

/* Append formatted text to buf[cap] starting at *off, clamping so *off never
 * exceeds cap-1. No-op once the buffer is full. Keeps multi-append loops safe. */
static void str_appendf(char *buf, size_t cap, int *off, const char *fmt, ...) {
    if (*off < 0) *off = 0;
    if ((size_t)*off >= cap) return;            /* full; nothing more fits */
    va_list ap;
    va_start(ap, fmt);
    int w = vsnprintf(buf + *off, cap - (size_t)*off, fmt, ap);
    va_end(ap);
    if (w < 0) return;                          /* encoding error; leave *off */
    *off += w;
    if ((size_t)*off >= cap) *off = (int)cap - 1; /* clamp to last valid index */
}
#endif /* !_WIN32 */

/* ── Sleep ──────────────────────────────────────────────────────────────── */

static void sleep_us(unsigned us) {
#ifdef _WIN32
    Sleep(us / 1000 + (us % 1000 ? 1 : 0));
#else
    struct timespec ts;
    ts.tv_sec  = us / 1000000;
    ts.tv_nsec = (long)(us % 1000000) * 1000L;
    nanosleep(&ts, NULL);
#endif
}

/* ── Live status line ───────────────────────────────────────────────────── */

/* Clear the current status line (TTY only) so an event/log line can be printed
 * cleanly above it. */
static void status_clear(void) {
    if (g_ui.status_tty)
        fprintf(stderr, "\r%s", g_ui.color ? "\x1b[K"
                : "                                                            \r");
}

/* Render the in-place status line. `tick` advances the spinner. */
static void status_render(bridge_t *b, uint64_t tick) {
    if (!g_ui.status_tty) return;
    char up[32], moves[32];
    ui_humanize_uptime((mono_ms() - b->start_ms) / 1000u, up, sizeof up);
    ui_group_thousands(b->ferrum_moves, moves, sizeof moves);
    const char *health = ui_link_health(b->probe_ok, b->probe_fail);
    const char *hcol = (health[0] == 'o') ? c_grn() :
                       (health[0] == 'd') ? c_red() : c_yel();
    const char *dot = g_ui.utf8 ? "\xc2\xb7" : "-";
    char spinbuf[4];
    if (g_ui.utf8) snprintf(spinbuf, sizeof spinbuf, "%s", ui_spinner_braille(tick));
    else { spinbuf[0] = ui_spinner_ascii(tick); spinbuf[1] = '\0'; }
    fprintf(stderr, "\r%s%s running%s %s %s %s moves %s link %s%s%s%s",
            c_dim(), spinbuf, c_rst(),
            up, dot, moves, dot,
            hcol, health, c_rst(),
            g_ui.color ? "\x1b[K" : "            ");
    fflush(stderr);
}

/* ── main ───────────────────────────────────────────────────────────────── */

int main(int argc, char **argv) {
    args_t args;
    if (parse_args(argc, argv, &args) != 0) return 2;

    ui_init(args.no_color);

    signal(SIGINT,  on_signal);
    signal(SIGTERM, on_signal);

    bridge_t br;
    memset(&br, 0, sizeof(br));
    br.request_timeout_ms = args.timeout_ms;

#ifndef _WIN32
    char auto_dev[256] = {0};
    bool device_auto = false;
    if (!args.device) {
        dev_cand_t cands[8];
        size_t nc = discover_devices(cands, 8);
        if (nc == 1) {
            snprintf(auto_dev, sizeof auto_dev, "%s", cands[0].path);
            args.device = auto_dev;
            device_auto = true;
        } else if (nc == 0) {
            return bridge_fail(2, "No serial devices found. Is the device plugged in?",
                "  (Looked for the usual /dev/cu.* / /dev/tty* serial ports.)\n"
                "  -> If it's plugged in but not listed, you may need the WCH CH34x driver.");
        } else {
            char list[512]; int o = 0;
            for (size_t i = 0; i < nc; i++)
                str_appendf(list, sizeof list, &o, "%s      %s%s",
                            i ? "\n" : "", cands[i].path,
                            cands[i].wch ? "   (WCH USB-serial)" : "");
            char body[700]; int bo = 0;
            str_appendf(body, sizeof body, &bo,
                "  Found several serial ports:\n%s\n"
                "  -> Re-run with one, e.g.:\n       hurra-bridge --device %s",
                list, cands[0].path);
            return bridge_fail(2, "No --device given, and found several serial ports", body);
        }
    }
#endif

    br.hc = hurra_open(args.device, args.baud);
    if (!br.hc) {
        char head[320], body[640];
#ifndef _WIN32
        int e = diag_open_errno(args.device);
        switch (ui_open_category(e)) {
        case UI_OPEN_NOENT: {
            dev_cand_t cands[8];
            size_t nc = discover_devices(cands, 8);
            char list[512] = "    (none found)";
            if (nc) {
                int o = 0;
                for (size_t i = 0; i < nc; i++)
                    str_appendf(list, sizeof list, &o, "%s    %s%s",
                                i ? "\n" : "", cands[i].path,
                                cands[i].wch ? "   (WCH USB-serial)" : "");
            }
            snprintf(head, sizeof head, "Can't open serial device: %s", args.device);
            snprintf(body, sizeof body,
                "  No such device. Is it plugged in?\n"
                "  -> Available serial ports:\n%s", list);
            break;
        }
        case UI_OPEN_ACCES:
            snprintf(head, sizeof head, "Permission denied: %s", args.device);
            snprintf(body, sizeof body,
                "  Your user isn't allowed to use this serial port.\n"
                "  -> Add yourself to the 'dialout' group, then log out and back in:\n"
                "       sudo usermod -aG dialout $USER");
            break;
        case UI_OPEN_BUSY:
            snprintf(head, sizeof head, "Device is in use: %s", args.device);
            snprintf(body, sizeof body,
                "  Another program (another bridge instance?) already holds this port.");
            break;
        default:
            snprintf(head, sizeof head, "Can't open serial device: %s", args.device);
            snprintf(body, sizeof body, "  %s", e ? strerror(e) : "open failed");
            break;
        }
#else
        snprintf(head, sizeof head, "Can't open serial device: %s", args.device);
        snprintf(body, sizeof body, "  The device couldn't be opened (in use, missing, or wrong COM name).");
#endif
        return bridge_fail(1, head, body);
    }

    /* Pack multiple small frames into one 64-byte USB transfer (CH343B MPS).
     * Flushed at the end of every loop tick for bounded latency. */
    hurra_set_tx_batch(br.hc, 64);

#ifdef _WIN32
    if (!args.virtual_port) {
        hurra_close(br.hc);
        return bridge_fail(1, "No --virtual-port given (required on Windows)",
            "  hurra-bridge needs a com0com virtual COM pair.\n"
            "  -> Install com0com, create a pair with setupc.exe (e.g. CNCA0 <-> CNCB0),\n"
            "     then run:  hurra-bridge.exe --device COM5 --virtual-port CNCA0");
    }
    br.vp = vp_open(args.virtual_port, NULL);
    if (!br.vp) {
        char body[256];
        snprintf(body, sizeof body,
            "  Couldn't open com0com port '%s' (GetLastError=%lu).\n"
            "  -> Is the name correct and the com0com pair installed?",
            args.virtual_port, (unsigned long)GetLastError());
        hurra_close(br.hc);
        return bridge_fail(1, "Can't open virtual COM port", body);
    }

    /* ---- Banner ---- */
    print_banner_head();
    {
        char baudbuf[32];
        ui_humanize_baud(args.baud, baudbuf, sizeof baudbuf);
        fprintf(stderr, "  %s%s%s Serial device   %s @ %s\n",
                c_grn(), g_ok(), c_rst(), args.device, baudbuf);
        fprintf(stderr, "  %s%s%s Virtual port    %s\n",
                c_grn(), g_ok(), c_rst(), args.virtual_port);
    }
#else
    char *owned_link = NULL;
    const char *link = args.link_path;
    if (!link) {
        owned_link = default_link_path();
        link = owned_link;
    }
    br.vp = vp_open(NULL, link);
    if (!br.vp) {
        free(owned_link);
        hurra_close(br.hc);
        return bridge_fail(1, "Can't create the virtual serial port (PTY)",
            "  The kernel refused to allocate a pseudo-terminal.\n"
            "  -> This is unusual; check ulimits and that /dev/ptmx is accessible.");
    }
    const char *slave = vp_slave_path(br.vp);

    /* ---- Banner ---- */
    print_banner_head();
    {
        char baudbuf[32];
        ui_humanize_baud(args.baud, baudbuf, sizeof baudbuf);
        fprintf(stderr, "  %s%s%s Serial device   %s @ %s%s\n",
                c_grn(), g_ok(), c_rst(), args.device, baudbuf,
                device_auto ? "  (auto-detected)" : "");
        fprintf(stderr, "  %s%s%s Virtual port    %s\n",
                c_grn(), g_ok(), c_rst(), slave ? slave : "(unknown)");
        if (link)
            fprintf(stderr, "    %s\xe2\x94\x94 linked at%s     %s\n",
                    c_dim(), c_rst(), link);
    }
    free(owned_link);
#endif

    /* Initial firmware probe — non-fatal. Updates probe counters. */
    {
        char fw[64] = {0};
        int rc = hurra_version(br.hc, fw, sizeof fw, 250);
        br.probe_calls++;
        if (rc == 0) {
            br.probe_ok++;
            fprintf(stderr, "  %s%s%s Firmware        responding (fw \"%s\")\n",
                    c_grn(), g_ok(), c_rst(), fw);
        } else {
            br.probe_fail++;
            fprintf(stderr, "  %s%s%s Firmware        not responding%s\n",
                    c_red(), g_bad(), c_rst(), "");
            fprintf(stderr, "%s"
                "    The serial port opened, but the device isn't answering.\n"
                "    Likely causes:\n"
                "      - Wrong baud rate (firmware default is 4 Mbaud; try without --baud)\n"
                "      - USB-serial driver too slow for 4 Mbaud\n"
                "        (macOS: install the WCH CH34x VCP driver)\n"
                "      - Wrong device - this port may be something else%s\n",
                c_dim(), c_rst());
        }
    }

    (void)hurra_on_telemetry(br.hc, HURRA_TYPE_TLM_BUTTONS, on_tlm_buttons, &br);
    (void)hurra_on_telemetry(br.hc, HURRA_TYPE_TLM_AXIS,    on_tlm_axis,    &br);
    (void)hurra_on_telemetry(br.hc, HURRA_TYPE_TLM_KB,      on_tlm_kb,      &br);

    ferrum_callbacks_t cbs;
    memset(&cbs, 0, sizeof(cbs));
    cbs.on_version        = cb_version;
    cbs.on_move           = cb_move;
    cbs.on_button_set     = cb_button_set;
    cbs.on_button_get     = cb_button_get;
    cbs.on_click          = cb_click;
    cbs.on_wheel          = cb_wheel;
    cbs.on_lock_set       = cb_lock_set;
    cbs.on_lock_get       = cb_lock_get;
    cbs.on_catch_xy       = cb_catch_xy;
    cbs.on_kb_down        = cb_kb_down;
    cbs.on_kb_up          = cb_kb_up;
    cbs.on_kb_press       = cb_kb_press;
    cbs.on_kb_multi       = cb_kb_multi;
    cbs.on_kb_isdown      = cb_kb_isdown;
    cbs.on_kb_mask_set    = cb_kb_mask_set;
    cbs.on_kb_mask_get    = cb_kb_mask_get;
    cbs.on_init           = cb_init;
    cbs.on_baud           = cb_baud;
    cbs.on_human          = cb_human;
    cbs.on_cb_buttons_set = cb_cb_buttons_set;
    cbs.on_cb_buttons_get = cb_cb_buttons_get;
    cbs.on_cb_axes_set    = cb_cb_axes_set;
    cbs.on_cb_axes_get    = cb_cb_axes_get;
    cbs.on_cb_keys_set    = cb_cb_keys_set;
    cbs.on_cb_keys_get    = cb_cb_keys_get;

    br.parser = ferrum_parser_create(&cbs, &br);
    if (!br.parser) {
        blog("error: ferrum_parser_create failed");
        vp_close(br.vp);
        hurra_close(br.hc);
        return 1;
    }

    br.start_ms = mono_ms();
#ifndef _WIN32
    {
        const char *slave2 = vp_slave_path(br.vp);
        fprintf(stderr, "\n  Ready. Point your Ferrum tool at %s\n",
                (args.link_path ? args.link_path :
                 (slave2 ? slave2 : "the PTY")));
    }
#else
    fprintf(stderr, "\n  Ready. Point your Ferrum tool at the other end of the com0com pair.\n");
#endif
    fprintf(stderr, "  Press Ctrl-C to stop.\n\n");
    fflush(stderr);

    uint8_t buf[256];
    /* __diag__ side-channel: intercept lines that exactly match "__diag__"
     * and reply with a health report. Real Ferrum apps will never send this. */
    char     diag_buf[16];
    uint8_t  diag_pos = 0;
    uint64_t last_heartbeat_ms = br.start_ms;
    const uint64_t HEARTBEAT_PERIOD_MS = 5000;
    uint64_t last_status_ms = br.start_ms;
    uint64_t spin_tick = 0;
    const uint64_t STATUS_PERIOD_MS = 125;
    const char *last_health = ui_link_health(br.probe_ok, br.probe_fail);

    while (!g_stop) {
        int n = vp_read(br.vp, buf, sizeof(buf));
        if (n < 0) {
            blog("vp_read error; exiting");
            break;
        }
        for (int i = 0; i < n; i++) {
            uint8_t c = buf[i];
            if (c == '\n') {
                if (diag_pos == 8 && memcmp(diag_buf, "__diag__", 8) == 0) {
                    uint64_t now = mono_ms();
                    uint64_t up  = now - br.start_ms;
                    char out[512];
                    int w = snprintf(out, sizeof(out),
                        "bridge_diag {\r\n"
                        "  uptime_ms=%llu\r\n"
                        "  probe_calls=%u  probe_ok=%u  probe_fail=%u\r\n"
                        "  ferrum_lines_in=%llu  ferrum_moves=%llu\r\n"
                        "  hurra_rx_bytes_total=%llu\r\n"
                        "  fw_link=%s\r\n"
                        "}\r\n",
                        (unsigned long long)up,
                        (unsigned)br.probe_calls,
                        (unsigned)br.probe_ok,
                        (unsigned)br.probe_fail,
                        (unsigned long long)br.ferrum_lines_in,
                        (unsigned long long)br.ferrum_moves,
                        (unsigned long long)br.hurra_rx_bytes,
                        (br.probe_fail == 0 && br.probe_ok > 0) ? "ok" :
                        (br.probe_ok == 0 && br.probe_fail > 0) ? "DEAD" :
                        (br.probe_ok > 0 && br.probe_fail > 0)  ? "flapping" :
                                                                  "unknown");
                    if (w > 0) vp_write_all(br.vp, (const uint8_t *)out, (size_t)w);
                    blog("__diag__ requested; replied %d bytes", w);
                }
                diag_pos = 0;
            } else if (c == '\r') {
                /* CR before LF is fine; just don't reset, the LF will. */
            } else if (diag_pos < sizeof(diag_buf)) {
                diag_buf[diag_pos++] = (char)c;
            } else {
                diag_pos = (uint8_t)sizeof(diag_buf);  /* line too long; clamp */
            }

            ferrum_parser_feed_byte(br.parser, c);
            if (c == '\n') br.ferrum_lines_in++;
        }
        ferrum_parser_tick(br.parser);

        int drained = hurra_poll(br.hc);
        if (drained < 0) {
            blog("hurra_poll error; exiting");
            break;
        }
        if (drained > 0) br.hurra_rx_bytes += (uint64_t)drained;

        uint64_t now = mono_ms();

        /* Detect link-health transitions and print an event line above the
         * status line so scrollback keeps a trail. */
        const char *health = ui_link_health(br.probe_ok, br.probe_fail);
        if (strcmp(health, last_health) != 0) {
            status_clear();
            if (strcmp(health, "ok") == 0)
                fprintf(stderr, "%s%s firmware responding%s\n", c_grn(), g_ok(), c_rst());
            else if (strcmp(health, "dead") == 0)
                fprintf(stderr, "%s%s firmware stopped responding%s\n", c_red(), g_bad(), c_rst());
            else if (strcmp(health, "flapping") == 0)
                fprintf(stderr, "%s~ firmware link is flapping%s\n", c_yel(), c_rst());
            fflush(stderr);
            last_health = health;
        }

        if (g_ui.status_tty) {
            if (now - last_status_ms >= STATUS_PERIOD_MS) {
                last_status_ms = now;
                status_render(&br, spin_tick++);
            }
        } else {
            /* Non-TTY: keep a periodic plain heartbeat line for logs. */
            if (now - last_heartbeat_ms >= HEARTBEAT_PERIOD_MS) {
                last_heartbeat_ms = now;
                char up[32], moves[32];
                ui_humanize_uptime((now - br.start_ms) / 1000u, up, sizeof up);
                ui_group_thousands(br.ferrum_moves, moves, sizeof moves);
                fprintf(stderr, "hurra-bridge: up %s, %s moves, link %s\n",
                        up, moves, ui_link_health(br.probe_ok, br.probe_fail));
                fflush(stderr);
            }
        }

        /* Flush batched TX each tick so the firmware sees commands within ~500us. */
        (void)hurra_flush(br.hc);

        if (n == 0 && drained == 0) {
            sleep_us(500);
        }
    }

    status_clear();
    if (g_ui.status_tty) fprintf(stderr, "\n");
    {
        char up[32], moves[32];
        ui_humanize_uptime((mono_ms() - br.start_ms) / 1000u, up, sizeof up);
        ui_group_thousands(br.ferrum_moves, moves, sizeof moves);
        const char *health = ui_link_health(br.probe_ok, br.probe_fail);
        const char *final_h = (strcmp(health, "ok") == 0)       ? "firmware link ok" :
                              (strcmp(health, "dead") == 0)     ? "firmware was unreachable" :
                              (strcmp(health, "flapping") == 0) ? "firmware link flapped" :
                                                                  "firmware link unknown";
        const char *dot = g_ui.utf8 ? "\xc2\xb7" : "-";
        fprintf(stderr, "Stopping hurra-bridge.\n  Ran for %s %s %s moves %s %s\n",
                up, dot, moves, dot, final_h);
        fflush(stderr);
    }
    ferrum_parser_destroy(br.parser);
    vp_close(br.vp);
    hurra_close(br.hc);
    return 0;
}
