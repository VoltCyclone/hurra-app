/*
 * bridge.c — hurra-bridge: endpoint ↔ hurra-on-serial bridge.
 *
 * Reads commands from a chosen frontend (Virtual COM port / PTY, or KMBox Net
 * UDP), translates them to Hurra binary frames over a real serial link to the
 * firmware. Telemetry from the firmware is routed back through the frontend.
 *
 * Single-threaded; the main loop pumps both transports with a short sleep
 * between iterations. The bridge is not latency-critical at the millisecond
 * scale — the firmware does the real work.
 */

#include "frontend.h"
#include "frontend_vcom.h"
#include "frontend_kmbox.h"
#include "input_core.h"
#include "selector.h"
#include "hurra.h"
#include "hurra_types.h"
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

#define KM_DEFAULT_PORT 16896

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

    int  request_timeout_ms;

    /* Diagnostics — bumped at hot-path points, dumped by the 5s heartbeat.
     * Single-threaded process; no synchronization needed.
     * uint64_t to handle long-running daemons. */
    uint64_t hurra_rx_bytes;
    uint64_t ferrum_lines_in;       /* lines successfully dispatched (kept for summary) */
    uint64_t ferrum_moves;          /* move count — will be 0 in rewired bridge (known limitation) */
    uint32_t probe_calls;
    uint32_t probe_ok;
    uint32_t probe_fail;
    uint64_t start_ms;              /* monotonic ms at bridge start */
} bridge_t;

/* ── Logging ────────────────────────────────────────────────────────────── */

static void blog(const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    vfprintf(stderr, fmt, ap);
    va_end(ap);
    fprintf(stderr, "\n");
    fflush(stderr);
}

/* Forward decl: defined just before main(); needed below to clear the live
 * status line before logging. */
static void status_clear(void);

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

/* ── Arg parsing ────────────────────────────────────────────────────────── */

static void usage(const char *prog) {
    fprintf(stderr,
        "hurra-bridge — bridge to Hurra firmware (VCOM or KMBox Net)\n"
        "\n"
        "usage: %s [--device PATH] [--baud N] [--link PATH]\n"
        "                   [--virtual-port NAME] [--timeout-ms N]\n"
        "                   [--km-port N] [--km-bind ADDR] [--km-mac HEX]\n"
        "                   [--no-color]\n"
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
        "  --km-port N          KMBox Net UDP port to listen on. Default %d.\n"
        "  --km-bind ADDR       KMBox Net bind address. Default 0.0.0.0.\n"
        "  --km-mac HEX         KMBox Net MAC filter (hex, 0 = accept any). Default 0.\n"
        "  --no-color           Disable colored output (also honors NO_COLOR env).\n"
        "  -h, --help           Show this help and exit.\n",
        prog, HOME_ENV, KM_DEFAULT_PORT);
}

typedef struct {
    const char *device;
    uint32_t    baud;
    const char *link_path;       /* Unix only */
    const char *virtual_port;    /* Windows only */
    int         timeout_ms;
    bool        no_color;        /* --no-color: force-disable color */
    /* KMBox Net options */
    const char *km_bind;
    uint16_t    km_port;
    uint32_t    km_mac;
} args_t;

static int parse_args(int argc, char **argv, args_t *out) {
    out->device = NULL;
    out->baud = 4000000;
    out->link_path = NULL;
    out->virtual_port = NULL;
    out->timeout_ms = 250;
    out->no_color = false;
    out->km_bind = "0.0.0.0";
    out->km_port = KM_DEFAULT_PORT;
    out->km_mac = 0;

    for (int i = 1; i < argc; i++) {
        const char *a = argv[i];
        if (!strcmp(a, "--device") && i + 1 < argc)       out->device = argv[++i];
        else if (!strcmp(a, "--baud") && i + 1 < argc)    out->baud = (uint32_t)strtoul(argv[++i], NULL, 10);
        else if (!strcmp(a, "--link") && i + 1 < argc)    out->link_path = argv[++i];
        else if (!strcmp(a, "--virtual-port") && i + 1 < argc) out->virtual_port = argv[++i];
        else if (!strcmp(a, "--timeout-ms") && i + 1 < argc)   out->timeout_ms = atoi(argv[++i]);
        else if (!strcmp(a, "--km-port") && i + 1 < argc) out->km_port = (uint16_t)strtoul(argv[++i], NULL, 10);
        else if (!strcmp(a, "--km-bind") && i + 1 < argc) out->km_bind = argv[++i];
        else if (!strcmp(a, "--km-mac") && i + 1 < argc)  out->km_mac = (uint32_t)strtoul(argv[++i], NULL, 16);
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

    /* ── Endpoint selector ───────────────────────────────────────────────── */
    int ep = selector_choose();
    if (ep < 0) {
        hurra_close(br.hc);
        return bridge_fail(2, "No terminal to choose an endpoint",
            "  hurra-bridge now starts with an interactive endpoint menu.\n"
            "  -> Run it from an interactive terminal (TTY required).");
    }

    /* ── Build sink + frontend ───────────────────────────────────────────── */
    input_sink_t sink;
    input_core_init(&sink, br.hc);
    sink.move_count = &br.ferrum_moves;
    frontend_t fe;
    memset(&fe, 0, sizeof fe);

    if (ep == ENDPOINT_KMBOX) {
        if (frontend_kmbox_open(&fe, &sink, args.km_bind, args.km_port, args.km_mac) != 0) {
            hurra_close(br.hc);
            return bridge_fail(1, "Can't bind KMBox Net UDP port",
                "  The UDP port is in use or not permitted. Try --km-port N.");
        }
    } else {
#ifdef _WIN32
        const char *link = NULL;
        const char *vp_arg = args.virtual_port;
        if (!vp_arg) {
            hurra_close(br.hc);
            return bridge_fail(1, "No --virtual-port given (required on Windows)",
                "  hurra-bridge needs a com0com virtual COM pair.\n"
                "  -> Install com0com, create a pair with setupc.exe (e.g. CNCA0 <-> CNCB0),\n"
                "     then run:  hurra-bridge.exe --device COM5 --virtual-port CNCA0");
        }
#else
        const char *vp_arg = NULL;
        char *owned_link = NULL;
        const char *link = args.link_path;
        if (!link) { owned_link = default_link_path(); link = owned_link; }
#endif
        if (frontend_vcom_open(&fe, &sink, br.hc, vp_arg, link, args.timeout_ms) != 0) {
            hurra_close(br.hc);
#ifndef _WIN32
            free(owned_link);
#endif
            return bridge_fail(1, "Can't create the virtual serial port (PTY)",
                "  The kernel refused to allocate a pseudo-terminal.");
        }
#ifndef _WIN32
        /* vp_open strdups link_path internally; owned_link no longer needed. */
        free(owned_link);
        owned_link = NULL;
#endif
    }

    /* ── Banner ─────────────────────────────────────────────────────────── */
    print_banner_head();
    {
        char baudbuf[32];
        ui_humanize_baud(args.baud, baudbuf, sizeof baudbuf);
#ifndef _WIN32
        fprintf(stderr, "  %s%s%s Serial device   %s @ %s%s\n",
                c_grn(), g_ok(), c_rst(), args.device, baudbuf,
                device_auto ? "  (auto-detected)" : "");
#else
        fprintf(stderr, "  %s%s%s Serial device   %s @ %s\n",
                c_grn(), g_ok(), c_rst(), args.device, baudbuf);
#endif
        fprintf(stderr, "  %s%s%s Endpoint        %s\n",
                c_grn(), g_ok(), c_rst(), fe.describe(&fe));
    }

    /* ── Initial firmware probe — non-fatal ─────────────────────────────── */
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

    br.start_ms = mono_ms();

    /* Ready message — endpoint-specific hint */
    if (ep == ENDPOINT_KMBOX) {
        fprintf(stderr, "\n  Ready. Point your KMBox Net tool at %s:%u\n",
                args.km_bind, (unsigned)args.km_port);
    } else {
#ifndef _WIN32
        const char *desc = fe.describe(&fe);
        fprintf(stderr, "\n  Ready. Point your Ferrum tool at %s\n",
                desc ? desc : "the PTY");
#else
        fprintf(stderr, "\n  Ready. Point your Ferrum tool at the other end of the com0com pair.\n");
#endif
    }
    fprintf(stderr, "  Press Ctrl-C to stop.\n\n");
    fflush(stderr);

    uint64_t last_heartbeat_ms = br.start_ms;
    const uint64_t HEARTBEAT_PERIOD_MS = 5000;
    uint64_t last_status_ms = br.start_ms;
    uint64_t spin_tick = 0;
    const uint64_t STATUS_PERIOD_MS = 125;
    const char *last_health = ui_link_health(br.probe_ok, br.probe_fail);

    while (!g_stop) {
        int did = fe.poll(&fe);
        if (did < 0) { blog("frontend poll error; exiting"); break; }

        int drained = hurra_poll(br.hc);
        if (drained < 0) {
            blog("hurra_poll error; exiting");
            break;
        }
        if (drained > 0) br.hurra_rx_bytes += (uint64_t)drained;

        uint64_t now = mono_ms();

        /* Detect link-health transitions and print an event line above the
         * status line so scrollback keeps a trail. */
        /* Link-health is a cumulative summary: once both an ok and a failure
         * have occurred it latches to "flapping" (it won't return to "ok").
         * Event lines therefore fire on first transition into each state. */
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

        if (did == 0 && drained == 0) {
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
    fe.close(&fe);
    hurra_close(br.hc);
    return 0;
}
