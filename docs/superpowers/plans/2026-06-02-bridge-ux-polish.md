# hurra-bridge UX Polish Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Make the `hurra-bridge` terminal experience polished and end-user-ready for non-expert operators: plain-English errors with fixes, a calm live status line with a spinner, a clean startup banner, and Unix device auto-discovery.

**Architecture:** All product UI logic lives in `src/bridge.c`, with pure (hardware-free, TTY-free) helpers factored into a new header-only `src/ui_util.h` so they can be unit-tested. Color/TTY/UTF-8 capability is detected once at startup and gates all decoration, with `NO_COLOR` / `--no-color` / non-TTY auto-disable. No `CMakeLists.txt` changes and no new link dependencies for the shipped binary.

**Tech Stack:** C99, POSIX (`isatty`, `glob`, `open`, `clock_gettime`), Win32 console VT (`GetConsoleMode`/`SetConsoleMode`), CMake (build only — untouched).

---

## Reference: spec

Design spec is at `docs/superpowers/specs/2026-06-02-bridge-ux-polish-design.md`. Read it before starting.

## File Structure

- **Create `src/ui_util.h`** — header-only `static inline` pure helpers: baud/uptime/number humanizers, open-errno classification, spinner frame, link-health label. No I/O, no globals. Included by `bridge.c` and by the test file.
- **Create `tests/ui_util_test.c`** — plain-C assertion tests for `ui_util.h`. Compiled ad-hoc with `cc` in test steps; NOT wired into CMake (keeps the spec's "no CMake changes" constraint and keeps test code out of the product).
- **Modify `src/bridge.c`** — `ui` state + `ui_init`, color/glyph emit helpers, `bridge_fail()` error formatter, `discover_devices()`, startup banner, live status line + event lines, shutdown summary, friendlier `usage()`, `--no-color`, optional `--device`. Uses `ui_util.h`.
- **Modify `src/serial_unix.c`** — silence the internal `fprintf` on open/configure failure (bridge now owns the message).
- **Modify `src/serial_win32.c`** — silence the internal `fprintf` on open/config failure (parity).
- **Modify `README.md`** — document `--no-color`, optional `--device` (Unix auto-detect), and refreshed output examples.

## Build & verify commands (used throughout)

- Configure once: `cmake -S . -B build`
- Build product: `cmake --build build` → produces `build/hurra-bridge`
- Compile helper tests ad-hoc: `cc -std=c99 -Wall -Wextra -Isrc -o build/ui_util_test tests/ui_util_test.c && ./build/ui_util_test`

---

## Task 1: Pure humanizer helpers + tests

**Files:**
- Create: `src/ui_util.h`
- Create: `tests/ui_util_test.c`

- [ ] **Step 1: Write the failing test**

Create `tests/ui_util_test.c`:

```c
/* Ad-hoc unit tests for src/ui_util.h pure helpers. Compile with:
 *   cc -std=c99 -Wall -Wextra -Isrc -o build/ui_util_test tests/ui_util_test.c
 */
#include "ui_util.h"
#include <stdio.h>
#include <string.h>

static int g_fail = 0;
#define CHECK_STR(expr, want) do {                                        \
    const char *got_ = (expr);                                            \
    if (strcmp(got_, (want)) != 0) {                                      \
        printf("FAIL %s:%d  %s == \"%s\", want \"%s\"\n",                 \
               __FILE__, __LINE__, #expr, got_, (want));                  \
        g_fail = 1;                                                       \
    }                                                                     \
} while (0)

static void test_humanize_baud(void) {
    char buf[32];
    CHECK_STR((ui_humanize_baud(4000000, buf, sizeof buf), buf), "4 Mbaud");
    CHECK_STR((ui_humanize_baud(2000000, buf, sizeof buf), buf), "2 Mbaud");
    CHECK_STR((ui_humanize_baud(115200,  buf, sizeof buf), buf), "115200 baud");
    CHECK_STR((ui_humanize_baud(1500000, buf, sizeof buf), buf), "1500000 baud");
}

static void test_humanize_uptime(void) {
    char buf[32];
    CHECK_STR((ui_humanize_uptime(0,    buf, sizeof buf), buf), "0s");
    CHECK_STR((ui_humanize_uptime(45,   buf, sizeof buf), buf), "45s");
    CHECK_STR((ui_humanize_uptime(84,   buf, sizeof buf), buf), "1m24s");
    CHECK_STR((ui_humanize_uptime(3661, buf, sizeof buf), buf), "1h01m01s");
}

static void test_group_thousands(void) {
    char buf[32];
    CHECK_STR((ui_group_thousands(0,     buf, sizeof buf), buf), "0");
    CHECK_STR((ui_group_thousands(12480, buf, sizeof buf), buf), "12,480");
    CHECK_STR((ui_group_thousands(1000,  buf, sizeof buf), buf), "1,000");
    CHECK_STR((ui_group_thousands(999,   buf, sizeof buf), buf), "999");
}

int main(void) {
    test_humanize_baud();
    test_humanize_uptime();
    test_group_thousands();
    if (g_fail) { printf("TESTS FAILED\n"); return 1; }
    printf("ALL TESTS PASSED\n");
    return 0;
}
```

- [ ] **Step 2: Run test to verify it fails**

Run: `cc -std=c99 -Wall -Wextra -Isrc -o build/ui_util_test tests/ui_util_test.c 2>&1 | head`
Expected: FAIL — compile error, `fatal error: ui_util.h: No such file or directory` (header not created yet).

- [ ] **Step 3: Write minimal implementation**

Create `src/ui_util.h`:

```c
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
```

- [ ] **Step 4: Run test to verify it passes**

Run: `cc -std=c99 -Wall -Wextra -Isrc -o build/ui_util_test tests/ui_util_test.c && ./build/ui_util_test`
Expected: `ALL TESTS PASSED`

- [ ] **Step 5: Commit**

```bash
git add src/ui_util.h tests/ui_util_test.c
git commit -m "feat: pure UI humanizer helpers with tests"
```

---

## Task 2: Open-errno classification + link-health + spinner helpers

**Files:**
- Modify: `src/ui_util.h`
- Modify: `tests/ui_util_test.c`

- [ ] **Step 1: Write the failing test**

Add to `tests/ui_util_test.c` above `main`, and call the new test functions from `main`:

```c
#include <errno.h>

static void test_classify_open(void) {
    if (ui_open_category(ENOENT) != UI_OPEN_NOENT) { printf("FAIL ENOENT\n"); g_fail = 1; }
    if (ui_open_category(EACCES) != UI_OPEN_ACCES) { printf("FAIL EACCES\n"); g_fail = 1; }
    if (ui_open_category(EBUSY)  != UI_OPEN_BUSY)  { printf("FAIL EBUSY\n");  g_fail = 1; }
    if (ui_open_category(EIO)    != UI_OPEN_OTHER) { printf("FAIL EIO\n");    g_fail = 1; }
}

static void test_link_health(void) {
    /* (ok, fail) counters -> label */
    CHECK_STR(ui_link_health(5, 0), "ok");
    CHECK_STR(ui_link_health(0, 3), "dead");
    CHECK_STR(ui_link_health(4, 2), "flapping");
    CHECK_STR(ui_link_health(0, 0), "unknown");
}

static void test_spinner(void) {
    /* ASCII spinner cycles through 4 frames deterministically by index. */
    if (ui_spinner_ascii(0) != '|') { printf("FAIL spin0\n"); g_fail = 1; }
    if (ui_spinner_ascii(1) != '/') { printf("FAIL spin1\n"); g_fail = 1; }
    if (ui_spinner_ascii(4) != '|') { printf("FAIL spin4 wrap\n"); g_fail = 1; }
}
```

And in `main`, before the `if (g_fail)` line, add:

```c
    test_classify_open();
    test_link_health();
    test_spinner();
```

- [ ] **Step 2: Run test to verify it fails**

Run: `cc -std=c99 -Wall -Wextra -Isrc -o build/ui_util_test tests/ui_util_test.c 2>&1 | head`
Expected: FAIL — compile error, `ui_open_category` / `ui_link_health` / `ui_spinner_ascii` / `UI_OPEN_NOENT` undeclared.

- [ ] **Step 3: Write minimal implementation**

Add to `src/ui_util.h` before the closing `#endif` (and add `#include <errno.h>` near the top includes):

```c
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
```

- [ ] **Step 4: Run test to verify it passes**

Run: `cc -std=c99 -Wall -Wextra -Isrc -o build/ui_util_test tests/ui_util_test.c && ./build/ui_util_test`
Expected: `ALL TESTS PASSED`

- [ ] **Step 5: Commit**

```bash
git add src/ui_util.h tests/ui_util_test.c
git commit -m "feat: errno classification, link-health, and spinner helpers"
```

---

## Task 3: UI state, capability detection, and color/glyph emit

**Files:**
- Modify: `src/bridge.c` (add includes near top; add the `ui` block after the `blog` definition around line 120)

- [ ] **Step 1: Add includes and the ui layer**

In `src/bridge.c`, add to the includes block (after `#include "virtual_port.h"`):

```c
#include "ui_util.h"
```

And in the Unix branch of the platform `#if` (where `<unistd.h>` is included) ensure `<unistd.h>` (already present) covers `isatty`; no change needed there.

After the `blog(...)` function (ends ~line 120), insert:

```c
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
static const char *g_ok(void)   { return g_ui.utf8 ? "\xe2\x9c\x93" : "*"; } /* ✓ */
static const char *g_bad(void)  { return g_ui.utf8 ? "\xe2\x9c\x97" : "x"; } /* ✗ */
```

- [ ] **Step 2: Wire `ui_init` into `main`**

In `main`, immediately after `if (parse_args(argc, argv, &args) != 0) return 2;` add:

```c
    ui_init(args.no_color);
```

(`args.no_color` is added in Task 4; if implementing strictly in order, temporarily pass `false` and update in Task 4. Prefer doing Task 4 next so this compiles.)

- [ ] **Step 3: Build to verify it compiles**

Run: `cmake --build build 2>&1 | tail -20`
Expected: builds `hurra-bridge` with no new errors. (If `args.no_color` is referenced before Task 4, either do Task 4 first or pass `false` here temporarily.)

- [ ] **Step 4: Commit**

```bash
git add src/bridge.c
git commit -m "feat: bridge UI capability detection and color/glyph helpers"
```

---

## Task 4: Friendlier usage, --no-color, optional --device

**Files:**
- Modify: `src/bridge.c` (`args_t`, `usage`, `parse_args` ~lines 339-381)

- [ ] **Step 1: Add `no_color` to `args_t`**

Replace the `args_t` struct (lines ~354-360) with:

```c
typedef struct {
    const char *device;
    uint32_t    baud;
    const char *link_path;       /* Unix only */
    const char *virtual_port;    /* Windows only */
    int         timeout_ms;
    bool        no_color;        /* --no-color: force-disable color */
} args_t;
```

- [ ] **Step 2: Rewrite `usage` to be friendlier and document new flags**

Replace `usage` (lines ~339-352) with:

```c
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
```

- [ ] **Step 3: Parse `--no-color`, make missing `--device` non-fatal on Unix**

In `parse_args`, set the default after `out->timeout_ms = 250;`:

```c
    out->no_color = false;
```

Add a flag branch alongside the others (before the `-h`/`--help` branch):

```c
        else if (!strcmp(a, "--no-color"))                out->no_color = true;
```

Replace the unknown-arg branch and the trailing device check (lines ~377-380) with:

```c
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
    /* On Unix, a missing --device triggers auto-discovery in main(). */
    return 0;
}
```

Note: `parse_args` is called before `ui_init`, so `c_red()`/`g_bad()` return based on the zero-initialized `g_ui` (color off) — acceptable; usage errors print plain. (Optional refinement: call `ui_init` first using a pre-scan for `--no-color`; not required.)

- [ ] **Step 4: Build to verify it compiles**

Run: `cmake --build build 2>&1 | tail -20`
Expected: builds cleanly.

- [ ] **Step 5: Manual check — help and bad flag**

Run: `./build/hurra-bridge --help; echo "exit=$?"`
Expected: friendly usage text, `exit=2`.
Run: `./build/hurra-bridge --bogus; echo "exit=$?"`
Expected: `unknown option: --bogus` then usage, `exit=2`.

- [ ] **Step 6: Commit**

```bash
git add src/bridge.c
git commit -m "feat: friendlier usage, --no-color flag, optional --device on Unix"
```

---

## Task 5: Device discovery and auto-select (Unix)

**Files:**
- Modify: `src/bridge.c` (add `discover_devices`; add `<glob.h>` on Unix)

- [ ] **Step 1: Add the discovery helper**

In the Unix branch of the top platform `#if` (where `<unistd.h>`/`<time.h>` are included), add:

```c
#  include <glob.h>
```

After `default_link_path()` (ends ~line 392), add:

```c
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
        glob_t gl;
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
#endif /* !_WIN32 */
```

- [ ] **Step 2: Build to verify it compiles**

Run: `cmake --build build 2>&1 | tail -20`
Expected: builds cleanly (function may be unused-warning-free since it's `static` and will be called in Task 6/7; if an unused warning appears, it is resolved when wired in Task 7).

- [ ] **Step 3: Manual smoke (best-effort, hardware-dependent)**

This helper is exercised end-to-end in Task 7's manual checks. For now, confirm it compiles. If serial hardware is present, the Task 7 auto-detect path will list it.

- [ ] **Step 4: Commit**

```bash
git add src/bridge.c
git commit -m "feat: Unix serial-device discovery helper"
```

---

## Task 6: Central error formatter + diagnosed open failures

**Files:**
- Modify: `src/bridge.c` (add `bridge_fail`, `diag_open_errno`; rewrite open/vp_open failure paths in `main`)
- Modify: `src/serial_unix.c` (silence internal fprintf)
- Modify: `src/serial_win32.c` (silence internal fprintf)

- [ ] **Step 1: Silence the serial layer's own error prints (Unix)**

In `src/serial_unix.c` `serial_open`, remove the three `fprintf(stderr, "serial_open(...)...")` lines (at ~97, ~103, ~109) so only the `return NULL` / cleanup remains. The bridge now owns user-facing messaging. Example — the open block becomes:

```c
    int fd = open(path, O_RDWR | O_NOCTTY | O_NONBLOCK);
    if (fd < 0) {
        return NULL;
    }
```

And each configure-failure block becomes just `{ close(fd); return NULL; }` (preserve `errno` by not calling anything between failure and return — `close` may change errno, but the bridge re-probes errno independently in Step 3, so this is fine).

- [ ] **Step 2: Silence the serial layer's own error prints (Windows)**

In `src/serial_win32.c`, remove the two `fprintf(stderr, "serial_open(...)...")` blocks (~42-43, ~68-69), keeping the `return NULL` / cleanup. Parity with Unix.

- [ ] **Step 3: Add `bridge_fail` and a fresh errno probe in `bridge.c`**

After the `ui_init`/color helpers block (end of Task 3 additions), add:

```c
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
 * the errno from a fresh O_RDWR|O_NONBLOCK open; 0 on success. */
static int diag_open_errno(const char *path) {
    errno = 0;
    int fd = open(path, O_RDWR | O_NOCTTY | O_NONBLOCK);
    if (fd >= 0) { close(fd); return 0; }
    return errno;
}
#endif
```

Add `#include <errno.h>` and (Unix) ensure `<fcntl.h>` is included for `open`/`O_RDWR`. In the Unix include branch add:

```c
#  include <fcntl.h>
```

- [ ] **Step 4: Rewrite the `hurra_open` failure path in `main`**

Replace the current block (lines ~420-425):

```c
    br.hc = hurra_open(args.device, args.baud);
    if (!br.hc) {
        blog("error: hurra_open(%s, %u) failed", args.device, (unsigned)args.baud);
        return 1;
    }
```

with (note: `args.device` may be NULL on Unix until Task 7 wires discovery; Task 7 guarantees it is set before this point):

```c
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
                    o += snprintf(list + o, sizeof(list) - o, "%s    %s%s",
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
```

Add `#include <string.h>` is already present. Ensure `dev_cand_t`/`discover_devices` (Task 5) precede this in the file (they do — Task 5 inserts them above `main`).

- [ ] **Step 5: Rewrite the `vp_open` / `--virtual-port` failure paths**

Windows branch — replace the `--virtual-port` required error (~432-437):

```c
    if (!args.virtual_port) {
        return bridge_fail(1, "No --virtual-port given (required on Windows)",
            "  hurra-bridge needs a com0com virtual COM pair.\n"
            "  -> Install com0com, create a pair with setupc.exe (e.g. CNCA0 <-> CNCB0),\n"
            "     then run:  hurra-bridge.exe --device COM5 --virtual-port CNCA0");
    }
```

Windows `vp_open` failure (~438-444):

```c
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
```

Unix `vp_open` failure (~453-459):

```c
    br.vp = vp_open(NULL, link);
    if (!br.vp) {
        free(owned_link);
        hurra_close(br.hc);
        return bridge_fail(1, "Can't create the virtual serial port (PTY)",
            "  The kernel refused to allocate a pseudo-terminal.\n"
            "  -> This is unusual; check ulimits and that /dev/ptmx is accessible.");
    }
```

- [ ] **Step 6: Build and run error-path checks**

Run: `cmake --build build 2>&1 | tail -20`
Expected: builds cleanly.

Run (no hardware needed): `./build/hurra-bridge --device /dev/cu.definitely-not-real; echo "exit=$?"`
Expected: `✗ Can't open serial device: /dev/cu.definitely-not-real`, then `No such device…` and an "Available serial ports" list (or `(none found)`), `exit=1`. No stray `serial_open(...)` line above it.

- [ ] **Step 7: Commit**

```bash
git add src/bridge.c src/serial_unix.c src/serial_win32.c
git commit -m "feat: diagnostic open/vp errors with fixes; silence raw serial logs"
```

---

## Task 7: Startup banner + non-fatal firmware probe + discovery wiring

**Files:**
- Modify: `src/bridge.c` (`main`: resolve device via discovery; banner; startup probe)

- [ ] **Step 1: Resolve `--device` via discovery before opening (Unix)**

Immediately after `ui_init(args.no_color);` and the `signal(...)` setup, before `hurra_open`, insert:

```c
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
                o += snprintf(list + o, sizeof(list) - o, "%s      %s%s",
                              i ? "\n" : "", cands[i].path,
                              cands[i].wch ? "   (WCH USB-serial)" : "");
            char body[700];
            snprintf(body, sizeof body,
                "  Found several serial ports:\n%s\n"
                "  -> Re-run with one, e.g.:\n       hurra-bridge --device %s",
                list, cands[0].path);
            return bridge_fail(2, "No --device given, and found several serial ports", body);
        }
    }
#endif
```

- [ ] **Step 2: Replace startup log lines with the banner header**

Replace `blog("hurra: opened %s @ %u baud", ...)` (~425) and the `tx_batch` log line (~430) so the open confirmation is folded into the banner. Keep the `hurra_set_tx_batch(br.hc, 64);` call but drop its `blog`. After `hurra_set_tx_batch(...)`, the old `blog("hurra: tx_batch=...")` line is deleted.

Add a banner helper after the color helpers (Task 3 region):

```c
/* Print the aligned startup banner (TTY) or plain lines (non-TTY). The
 * firmware line is filled by the caller after the probe. */
static void print_banner_head(void) {
    if (g_ui.status_tty) fprintf(stderr, "\nhurra-bridge\n\n");
    else                 fprintf(stderr, "hurra-bridge: starting\n");
}
```

- [ ] **Step 3: Emit device + port + firmware lines, run the probe**

Replace the Unix port-announcement block (the `printf("PTY: ...")` / `printf("Symlink: ...")` lines ~460-464) and the Windows `blog("vp: opened %s", ...)` (~445) with banner lines. Use this unified block placed right after the virtual port is successfully opened (both platforms), replacing those platform prints:

```c
    /* ---- Banner ---- */
    print_banner_head();

    char baudbuf[32];
    ui_humanize_baud(args.baud, baudbuf, sizeof baudbuf);

#ifndef _WIN32
    fprintf(stderr, "  %s%s%s Serial device   %s @ %s%s\n",
            c_grn(), g_ok(), c_rst(), args.device, baudbuf,
            device_auto ? "  (auto-detected)" : "");
    const char *slave = vp_slave_path(br.vp);
    fprintf(stderr, "  %s%s%s Virtual port    %s\n",
            c_grn(), g_ok(), c_rst(), slave ? slave : "(unknown)");
    if (link)
        fprintf(stderr, "    %s\xe2\x94\x94 linked at%s     %s\n",
                c_dim(), c_rst(), link);
#else
    fprintf(stderr, "  %s%s%s Serial device   %s @ %s\n",
            c_grn(), g_ok(), c_rst(), args.device, baudbuf);
    fprintf(stderr, "  %s%s%s Virtual port    %s\n",
            c_grn(), g_ok(), c_rst(), args.virtual_port);
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
```

Then replace the old `blog("bridge: running. SIGINT to stop.")` (~508) with the ready footer:

```c
#ifndef _WIN32
    fprintf(stderr, "\n  Ready. Point your Ferrum tool at %s\n",
            link ? link : (vp_slave_path(br.vp) ? vp_slave_path(br.vp) : "the PTY"));
#else
    fprintf(stderr, "\n  Ready. Point your Ferrum tool at the other end of the com0com pair.\n");
#endif
    fprintf(stderr, "  Press Ctrl-C to stop.\n\n");
    fflush(stderr);
```

Note: `link` is declared in the Unix `#else` branch of the existing port-open code; ensure the banner block on Unix is inside scope where `link` is visible (it is — that block lives after `link` is assigned). If scoping is awkward, hoist `const char *link = NULL;` to the top of the Unix section.

Remove the now-unused `blog`-based `cb_version` probe duplication? No — leave `cb_version` as is; it still serves runtime `km.version` calls.

- [ ] **Step 4: Build and run the banner (non-fatal firmware path)**

Run: `cmake --build build 2>&1 | tail -20`
Expected: builds cleanly.

Run (no firmware; uses a throwaway PTY via socat if available, else just observe the firmware-not-responding branch with a fake device that opens). Simplest hardware-free check on Linux:
`socat -d -d pty,raw,echo=0 pty,raw,echo=0 &` then point `--device` at one end:
`./build/hurra-bridge --device /dev/pts/<n>` (use the path socat printed).
Expected: banner prints with `✓ Serial device`, `✓ Virtual port`, and `✗ Firmware not responding` + remediation, then `Ready.`; Ctrl-C to stop.

- [ ] **Step 5: Commit**

```bash
git add src/bridge.c
git commit -m "feat: startup banner, device auto-detect wiring, non-fatal firmware probe"
```

---

## Task 8: Live status line + spinner + event lines

**Files:**
- Modify: `src/bridge.c` (`main` loop: replace heartbeat block ~574-589)

- [ ] **Step 1: Add status-render helpers**

After the banner helper (Task 7), add:

```c
/* Clear the current status line (TTY only) so an event/log line can be printed
 * cleanly above it. */
static void status_clear(void) {
    if (g_ui.status_tty) fprintf(stderr, "\r%s", g_ui.color ? "\x1b[K" : "                                                            \r");
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
    const char *spin = g_ui.utf8 ? ui_spinner_braille(tick) : NULL;
    char spinbuf[2];
    if (!spin) { spinbuf[0] = ui_spinner_ascii(tick); spinbuf[1] = '\0'; spin = spinbuf; }
    fprintf(stderr, "\r%s%s running %s %s %s %s moves %s link %s%s%s%s",
            c_dim(), spin, c_rst(),
            up, "\xc2\xb7" /*·*/ , moves, "\xc2\xb7",
            hcol, health, c_rst(),
            g_ui.color ? "\x1b[K" : "   ");
    fflush(stderr);
}
```

Note: when `g_ui.utf8` is false the `·` bytes still print as raw chars; to stay strictly ASCII, gate the separator: replace `"\xc2\xb7"` usages with `g_ui.utf8 ? "\xc2\xb7" : "-"`. Apply that gate in the `fprintf` (use a local `const char *dot = g_ui.utf8 ? "\xc2\xb7" : "-";`).

Revised body (use this exact version):

```c
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
            g_ui.color ? "\x1b[K" : "   ");
    fflush(stderr);
}
```

- [ ] **Step 2: Track link state + spinner tick in the loop; replace the heartbeat**

In `main`, before the `while (!g_stop)` loop, add locals:

```c
    uint64_t last_status_ms = br.start_ms;
    uint64_t spin_tick = 0;
    const uint64_t STATUS_PERIOD_MS = 125;
    const char *last_health = ui_link_health(br.probe_ok, br.probe_fail);
```

Replace the heartbeat block (lines ~574-589, the `if (now - last_heartbeat_ms >= HEARTBEAT_PERIOD_MS) { ... }`) with:

```c
        uint64_t now = mono_ms();

        /* Detect link-health transitions and print an event line above the
         * status line so scrollback keeps a trail. */
        const char *health = ui_link_health(br.probe_ok, br.probe_fail);
        if (health != last_health && strcmp(health, last_health) != 0) {
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
```

Keep the existing `last_heartbeat_ms`/`HEARTBEAT_PERIOD_MS` declarations (still used by the non-TTY branch).

- [ ] **Step 3: Quiet the per-move log (it now lives in the status line)**

In `cb_move` (lines ~186-200), remove the rate-limited `blog("move(...)")` block (the `if (b->ferrum_moves <= 10 || ...) { blog(...); }`). Keep the counter increment and the `hurra_move` call + its error log. The status line shows the move count instead.

- [ ] **Step 4: Build and run the live status line**

Run: `cmake --build build 2>&1 | tail -20`
Expected: builds cleanly.

Run (socat PTY as in Task 7): `./build/hurra-bridge --device /dev/pts/<n>`
Expected: after the banner, a single line `⠋ running 3s · 0 moves · link ✗` that animates in place (spinner advances, uptime climbs) and does NOT scroll. Piping `| cat` instead shows plain `hurra-bridge: up …` lines every 5s with no escapes.

- [ ] **Step 5: Commit**

```bash
git add src/bridge.c
git commit -m "feat: live status line with spinner and link-transition events"
```

---

## Task 9: Shutdown summary

**Files:**
- Modify: `src/bridge.c` (`main`: shutdown block ~599)

- [ ] **Step 1: Replace `blog("bridge: stopping.")` with a clean summary**

Replace the shutdown line (`blog("bridge: stopping.");`, ~599) with:

```c
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
        fprintf(stderr, "Stopping hurra-bridge.\n  Ran for %s %s %s moves %s %s\n",
                up, g_ui.utf8 ? "\xc2\xb7" : "-", moves,
                g_ui.utf8 ? "\xc2\xb7" : "-", final_h);
        fflush(stderr);
    }
```

- [ ] **Step 2: Build and verify clean shutdown**

Run: `cmake --build build 2>&1 | tail -20`
Expected: builds cleanly.

Run (socat PTY): start the bridge, let it run a few seconds, press Ctrl-C.
Expected: the spinner line is finalized with a newline (no half-drawn frame), followed by:
```
Stopping hurra-bridge.
  Ran for 6s · 0 moves · firmware was unreachable
```

- [ ] **Step 3: Commit**

```bash
git add src/bridge.c
git commit -m "feat: clean shutdown summary"
```

---

## Task 10: README refresh + full verification sweep

**Files:**
- Modify: `README.md`

- [ ] **Step 1: Update the flags table and quickstart**

In `README.md`:
- Add a `--no-color` row to the flags table:
  `| `--no-color` | _off_ | Disable colored output (also honors `NO_COLOR`). |`
- Change the `--device` row to note it is optional on Unix:
  `| `--device PATH` | _auto on Unix_ | Real serial device. Auto-detected when exactly one port is present; required on Windows. |`
- Replace the macOS/Linux quickstart sample output to reflect the new banner (device/virtual-port/firmware lines + `Ready.`), and mention the live status line and that output degrades to plain lines when piped or when `NO_COLOR`/`--no-color` is set.

- [ ] **Step 2: Full build matrix sanity (local platform)**

Run: `cmake --build build 2>&1 | tail -20`
Expected: clean build.

Run: `cc -std=c99 -Wall -Wextra -Isrc -o build/ui_util_test tests/ui_util_test.c && ./build/ui_util_test`
Expected: `ALL TESTS PASSED`

- [ ] **Step 3: Manual verification checklist (record results)**

Perform each and confirm (use socat PTY pair for the device on Linux/macOS):
1. TTY happy path: banner aligns; spinner animates; status line refreshes in place, no scroll; Ctrl-C → clean summary.
2. `./build/hurra-bridge --device <pty> | cat`: no escape codes; plain periodic lines; greppable banner + summary.
3. `NO_COLOR=1 ./build/hurra-bridge --device <pty>`: no escapes; ASCII spinner/markers; alignment intact.
4. `./build/hurra-bridge --no-color --device <pty>`: same as (3).
5. `./build/hurra-bridge --device /dev/nope`: `✗ Can't open…` + "Available serial ports" list; `exit=1`; no raw `serial_open(...)` line.
6. Missing `--device` with zero ports: `✗ No serial devices found…`; `exit=2`. With multiple: list + re-run hint; `exit=2`. With exactly one: auto-detected banner line.
7. `--bogus`: `✗ unknown option`; usage; `exit=2`. `--help`: usage; `exit=2` is acceptable (matches prior behavior; help via `-h` returns from parse with code 2 by current design — leave as is unless you intend to change it).
8. Firmware-silent: status shows `link ✗`; if firmware answers later, an event line `✓ firmware responding` prints and status flips to `link ✓`.

- [ ] **Step 4: Commit**

```bash
git add README.md
git commit -m "docs: document --no-color, Unix device auto-detect, and new output"
```

---

## Self-Review notes (resolved)

- **Spec coverage:** Section 1 → Task 3; Section 2 (banner) → Task 7; Section 3 (status line) → Task 8; Section 4 (errors) → Task 6; Section 4b (discovery) → Tasks 5+7; Section 5 (shutdown) → Task 9; `--no-color`/usage → Task 4; README → Task 10. Humanizers/classifiers → Tasks 1–2.
- **errno open item:** resolved via `diag_open_errno` fresh-probe in Task 6 (errno is clobbered on the real path).
- **clear-to-EOL open item:** resolved — `\x1b[K` when VT/color on, space-padding fallback otherwise (`status_clear`/`status_render`).
- **Braille vs ASCII:** gated on `g_ui.utf8`, which is only true when color is on and locale is UTF-8 (Unix) and never on Windows.
- **Type consistency:** helper names used in later tasks (`ui_humanize_baud`, `ui_humanize_uptime`, `ui_group_thousands`, `ui_open_category`, `ui_link_health`, `ui_spinner_ascii`, `ui_spinner_braille`, `bridge_fail`, `status_clear`, `status_render`, `discover_devices`, `dev_cand_t`) match their definitions in earlier tasks.
- **Known minor:** `--help`/`-h` returns exit code 2 (inherited from current `parse_args` design). Left intentionally to avoid scope creep; Task 10 step 3.7 notes it.
