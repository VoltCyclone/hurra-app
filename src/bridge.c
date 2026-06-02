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
#  define HOME_ENV "USERPROFILE"
#else
#  include <unistd.h>
#  include <time.h>
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
    /* Rate-limited log: first 10 moves, then every 256th. */
    if (b->ferrum_moves <= 10 || (b->ferrum_moves & 0xFF) == 0) {
        blog("move(%d, %d)  [seq=%llu]", (int)x, (int)y,
             (unsigned long long)b->ferrum_moves - 1);
    }
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
        "usage: %s --device PATH [--baud N] [--link PATH] [--virtual-port NAME]\n"
        "\n"
        "  --device PATH        Real serial device path (e.g. /dev/cu.usbmodem01,\n"
        "                       COM5). Required.\n"
        "  --baud N             Real serial baud rate. Default 4000000.\n"
        "  --link PATH          Symlink to point at the PTY slave (Unix only).\n"
        "                       Default $%s/.hurra-bridge.tty.\n"
        "  --virtual-port NAME  com0com COM name the bridge opens (Windows).\n"
        "                       Ignored on Unix.\n"
        "  --timeout-ms N       Per-request timeout for get-style commands. Default 250.\n",
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
        else if (!strcmp(a, "-h") || !strcmp(a, "--help")) { usage(argv[0]); return -1; }
        else { fprintf(stderr, "unknown arg: %s\n", a); usage(argv[0]); return -1; }
    }
    if (!out->device) { fprintf(stderr, "missing --device\n"); usage(argv[0]); return -1; }
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

    br.hc = hurra_open(args.device, args.baud);
    if (!br.hc) {
        blog("error: hurra_open(%s, %u) failed", args.device, (unsigned)args.baud);
        return 1;
    }
    blog("hurra: opened %s @ %u baud", args.device, (unsigned)args.baud);

    /* Pack multiple small frames into one 64-byte USB transfer (CH343B MPS).
     * Flushed at the end of every loop tick for bounded latency. */
    hurra_set_tx_batch(br.hc, 64);
    blog("hurra: tx_batch=64 bytes (CH343B MPS); flushed every main-loop tick");

#ifdef _WIN32
    if (!args.virtual_port) {
        blog("error: --virtual-port is required on Windows");
        hurra_close(br.hc);
        return 1;
    }
    br.vp = vp_open(args.virtual_port, NULL);
    if (!br.vp) {
        blog("error: vp_open(%s) failed (GetLastError=%lu)",
             args.virtual_port, (unsigned long)GetLastError());
        hurra_close(br.hc);
        return 1;
    }
    blog("vp: opened %s", args.virtual_port);
#else
    char *owned_link = NULL;
    const char *link = args.link_path;
    if (!link) {
        owned_link = default_link_path();
        link = owned_link;
    }
    br.vp = vp_open(NULL, link);
    if (!br.vp) {
        blog("error: vp_open failed");
        free(owned_link);
        hurra_close(br.hc);
        return 1;
    }
    const char *slave = vp_slave_path(br.vp);
    printf("PTY: %s\n", slave ? slave : "(unknown)");
    if (link) printf("Symlink: %s\n", link);
    fflush(stdout);
    free(owned_link);
#endif

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
    blog("bridge: running. SIGINT to stop.");

    uint8_t buf[256];
    /* __diag__ side-channel: intercept lines that exactly match "__diag__"
     * and reply with a health report. Real Ferrum apps will never send this. */
    char     diag_buf[16];
    uint8_t  diag_pos = 0;
    uint64_t last_heartbeat_ms = br.start_ms;
    const uint64_t HEARTBEAT_PERIOD_MS = 5000;

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
        if (now - last_heartbeat_ms >= HEARTBEAT_PERIOD_MS) {
            last_heartbeat_ms = now;
            blog("heartbeat up=%llus  moves=%llu  probes=%u(ok=%u fail=%u)  "
                 "rx_bytes=%llu  fw=%s",
                 (unsigned long long)((now - br.start_ms) / 1000),
                 (unsigned long long)br.ferrum_moves,
                 (unsigned)br.probe_calls,
                 (unsigned)br.probe_ok,
                 (unsigned)br.probe_fail,
                 (unsigned long long)br.hurra_rx_bytes,
                 (br.probe_fail == 0 && br.probe_ok > 0) ? "ok" :
                 (br.probe_ok == 0 && br.probe_fail > 0) ? "DEAD" :
                 (br.probe_ok > 0 && br.probe_fail > 0)  ? "flapping" :
                                                           "unknown");
        }

        /* Flush batched TX each tick so the firmware sees commands within ~500us. */
        (void)hurra_flush(br.hc);

        if (n == 0 && drained == 0) {
            sleep_us(500);
        }
    }

    blog("bridge: stopping.");
    ferrum_parser_destroy(br.parser);
    vp_close(br.vp);
    hurra_close(br.hc);
    return 0;
}
