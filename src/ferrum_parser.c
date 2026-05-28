/*
 * ferrum_parser.c — host port of imxrtnsy/src/ferrum.c.
 *
 * Line-buffered tokenizer with a 25 ms idle-gap reset to discard partial
 * lines after a baud mismatch. Action layer is replaced by a callback
 * table; emit_pair / emit_bool / emit_buttons_cb / emit_axes_cb /
 * emit_keys_cb are exposed for the bridge to produce reply text.
 */

#include "ferrum_parser.h"

#include <stdbool.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#ifdef _WIN32
#  define WIN32_LEAN_AND_MEAN
#  include <windows.h>
#endif

#define FERRUM_LINE_MAX    128
#define FERRUM_MAX_ARGS    8
#define FERRUM_IDLE_GAP_MS 25u

/* ── Monotonic ms ───────────────────────────────────────────────────────── */

static uint32_t now_ms(void) {
#ifdef _WIN32
    return (uint32_t)GetTickCount64();
#else
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint32_t)((uint64_t)ts.tv_sec * 1000ULL +
                      (uint64_t)ts.tv_nsec / 1000000ULL);
#endif
}

/* ── Helpers ────────────────────────────────────────────────────────────── */

static bool parse_int(const char *s, uint8_t len, int32_t *out) {
    if (len == 0) return false;
    bool neg = false;
    uint8_t i = 0;
    if (s[0] == '-') { neg = true; i = 1; }
    else if (s[0] == '+') { i = 1; }
    if (i >= len) return false;
    uint32_t u = 0;
    for (; i < len; i++) {
        char c = s[i];
        if (c < '0' || c > '9') return false;
        uint32_t d = (uint32_t)(c - '0');
        if (u > (0xFFFFFFFFu - d) / 10u) return false;
        u = u * 10u + d;
    }
    if (neg) {
        if (u > 0x80000000u) return false;
        *out = -(int32_t)u;
    } else {
        if (u > 0x7FFFFFFFu) return false;
        *out = (int32_t)u;
    }
    return true;
}

static bool parse_bool(const char *s, uint8_t len, bool *out) {
    if (len == 4 && s[0] == 't' && s[1] == 'r' && s[2] == 'u' && s[3] == 'e') {
        *out = true; return true;
    }
    if (len == 5 && s[0] == 'f' && s[1] == 'a' && s[2] == 'l' &&
        s[3] == 's' && s[4] == 'e') {
        *out = false; return true;
    }
    return false;
}

size_t ferrum_format_int(int32_t v, char *buf) {
    char tmp[12];
    uint8_t n = 0;
    bool neg = false;
    uint32_t u;
    if (v < 0) { neg = true; u = (uint32_t)(-(int64_t)v); }
    else       u = (uint32_t)v;
    if (u == 0) tmp[n++] = '0';
    while (u) { tmp[n++] = (char)('0' + (u % 10u)); u /= 10u; }
    size_t out = 0;
    if (neg) buf[out++] = '-';
    while (n) buf[out++] = tmp[--n];
    return out;
}

/* ── Text emitters ──────────────────────────────────────────────────────── */

void ferrum_emit_version_text(ferrum_write_fn w, void *u) {
    static const uint8_t s[] = "kmbox: Ferrum\r\n";
    if (w) w(s, sizeof(s) - 1, u);
}

void ferrum_emit_bool(ferrum_write_fn w, void *u, int v) {
    uint8_t b[3] = { (uint8_t)(v ? '1' : '0'), '\r', '\n' };
    if (w) w(b, 3, u);
}

void ferrum_emit_pair(ferrum_write_fn w, void *u, int32_t x, int32_t y) {
    char buf[40];
    size_t n = 0;
    buf[n++] = '(';
    n += ferrum_format_int(x, &buf[n]);
    buf[n++] = ','; buf[n++] = ' ';
    n += ferrum_format_int(y, &buf[n]);
    buf[n++] = ')'; buf[n++] = '\r'; buf[n++] = '\n';
    if (w) w((const uint8_t *)buf, n, u);
}

void ferrum_emit_buttons_cb(ferrum_write_fn w, void *u, uint8_t bitmap) {
    uint8_t buf[6] = { 'k', 'm', '.', bitmap, '\r', '\n' };
    if (w) w(buf, 6, u);
}

void ferrum_emit_axes_cb(ferrum_write_fn w, void *u,
                         int32_t dx, int32_t dy, int32_t scroll) {
    char buf[64];
    size_t n = 0;
    const char *prefix = "Axes(";
    for (; *prefix; prefix++) buf[n++] = *prefix;
    n += ferrum_format_int(dx, &buf[n]);
    buf[n++] = ','; buf[n++] = ' ';
    n += ferrum_format_int(dy, &buf[n]);
    buf[n++] = ','; buf[n++] = ' ';
    n += ferrum_format_int(scroll, &buf[n]);
    buf[n++] = ')'; buf[n++] = '\r'; buf[n++] = '\n';
    if (w) w((const uint8_t *)buf, n, u);
}

void ferrum_emit_keys_cb(ferrum_write_fn w, void *u, const uint8_t keys[6]) {
    /* Sort ascending. */
    uint8_t sorted[6];
    memcpy(sorted, keys, 6);
    for (uint8_t i = 1; i < 6; i++) {
        uint8_t v = sorted[i];
        int8_t j = (int8_t)i - 1;
        while (j >= 0 && sorted[j] > v) {
            sorted[j + 1] = sorted[j];
            j--;
        }
        sorted[j + 1] = v;
    }

    char buf[64];
    size_t n = 0;
    const char *prefix = "Keys(";
    for (; *prefix; prefix++) buf[n++] = *prefix;
    bool first = true;
    for (uint8_t i = 0; i < 6; i++) {
        if (sorted[i] == 0) continue;
        if (!first) { buf[n++] = ','; buf[n++] = ' '; }
        n += ferrum_format_int(sorted[i], &buf[n]);
        first = false;
    }
    buf[n++] = ')'; buf[n++] = '\r'; buf[n++] = '\n';
    if (w) w((const uint8_t *)buf, n, u);
}

/* ── Argument tokenizer ─────────────────────────────────────────────────── */

typedef struct {
    const char *p;
    uint8_t     len;
} arg_t;

static void trim(const char **p, uint8_t *len) {
    const char *s = *p;
    uint8_t n = *len;
    while (n && (*s == ' ' || *s == '\t')) { s++; n--; }
    while (n && (s[n - 1] == ' ' || s[n - 1] == '\t')) n--;
    *p = s; *len = n;
}

static uint8_t split_args(const char *s, uint8_t len, arg_t *args) {
    uint8_t n = 0;
    uint8_t start = 0;
    for (uint8_t i = 0; i <= len; i++) {
        if (i == len || s[i] == ',') {
            if (n >= FERRUM_MAX_ARGS) return n;
            const char *p = &s[start];
            uint8_t l = i - start;
            trim(&p, &l);
            args[n].p = p;
            args[n].len = l;
            n++;
            start = i + 1;
        }
    }
    if (n == 1 && args[0].len == 0) return 0;
    return n;
}

/* ── Parser state ───────────────────────────────────────────────────────── */

struct ferrum_parser {
    ferrum_callbacks_t cbs;
    void              *user;

    char     line[FERRUM_LINE_MAX];
    uint8_t  line_pos;
    bool     overflow;
    uint32_t last_byte_ms;
};

ferrum_parser_t *ferrum_parser_create(const ferrum_callbacks_t *cbs, void *user) {
    ferrum_parser_t *p = (ferrum_parser_t *)calloc(1, sizeof(*p));
    if (!p) return NULL;
    if (cbs) p->cbs = *cbs;
    p->user = user;
    return p;
}

void ferrum_parser_destroy(ferrum_parser_t *p) {
    free(p);
}

/* ── Dispatch ───────────────────────────────────────────────────────────── */

static inline bool name_is(const char *s, uint8_t len, const char *kw) {
    uint8_t kl = (uint8_t)strlen(kw);
    return len == kl && memcmp(s, kw, kl) == 0;
}

static void cmd_move(ferrum_parser_t *p, arg_t *args, uint8_t nargs) {
    if (nargs != 2) return;
    int32_t x, y;
    if (!parse_int(args[0].p, args[0].len, &x)) return;
    if (!parse_int(args[1].p, args[1].len, &y)) return;
    if (p->cbs.on_move) p->cbs.on_move(x, y, p->user);
}

static void cmd_button(ferrum_parser_t *p, uint8_t mask,
                       arg_t *args, uint8_t nargs) {
    if (nargs == 0) {
        if (p->cbs.on_button_get) p->cbs.on_button_get(mask, p->user);
        return;
    }
    if (nargs != 1) return;
    int32_t s;
    if (!parse_int(args[0].p, args[0].len, &s)) return;
    if (s != 0 && s != 1) return;
    if (p->cbs.on_button_set) p->cbs.on_button_set(mask, (uint8_t)s, p->user);
}

static void cmd_click(ferrum_parser_t *p, arg_t *args, uint8_t nargs) {
    if (nargs != 1) return;
    int32_t b;
    if (!parse_int(args[0].p, args[0].len, &b)) return;
    if (b < 0 || b > 4) return;
    if (p->cbs.on_click) p->cbs.on_click((uint8_t)b, p->user);
}

static void cmd_wheel(ferrum_parser_t *p, arg_t *args, uint8_t nargs) {
    if (nargs != 1) return;
    int32_t n;
    if (!parse_int(args[0].p, args[0].len, &n)) return;
    if (p->cbs.on_wheel) p->cbs.on_wheel(n, p->user);
}

static void cmd_lock(ferrum_parser_t *p, const char *name,
                     arg_t *args, uint8_t nargs) {
    if (nargs == 0) {
        if (p->cbs.on_lock_get) p->cbs.on_lock_get(name, p->user);
        return;
    }
    if (nargs != 1) return;
    int32_t s;
    if (!parse_int(args[0].p, args[0].len, &s)) return;
    if (s != 0 && s != 1) return;
    if (p->cbs.on_lock_set) p->cbs.on_lock_set(name, (uint8_t)s, p->user);
}

static void cmd_catch_xy(ferrum_parser_t *p, arg_t *args, uint8_t nargs) {
    if (nargs < 1 || nargs > 2) return;
    int32_t dur;
    if (!parse_int(args[0].p, args[0].len, &dur)) return;
    if (dur < 0) dur = 0;
    if (dur > 1000) dur = 1000;
    if (nargs == 2) {
        bool inc;
        if (!parse_bool(args[1].p, args[1].len, &inc)) return;
        (void)inc;
    }
    if (p->cbs.on_catch_xy) p->cbs.on_catch_xy((uint32_t)dur, p->user);
}

static void cmd_kb_simple(ferrum_parser_t *p, arg_t *args, uint8_t nargs,
                          int which /* 0=down 1=up 2=press */) {
    if (nargs != 1) return;
    int32_t k;
    if (!parse_int(args[0].p, args[0].len, &k)) return;
    uint8_t h = (uint8_t)k;
    if (which == 0 && p->cbs.on_kb_down)  p->cbs.on_kb_down(h,  p->user);
    if (which == 1 && p->cbs.on_kb_up)    p->cbs.on_kb_up(h,    p->user);
    if (which == 2 && p->cbs.on_kb_press) p->cbs.on_kb_press(h, p->user);
}

static void cmd_kb_multi(ferrum_parser_t *p, arg_t *args, uint8_t nargs,
                         int op) {
    if (nargs == 0 || nargs > 6) return;
    uint8_t keys[6];
    for (uint8_t i = 0; i < nargs; i++) {
        int32_t k;
        if (!parse_int(args[i].p, args[i].len, &k)) return;
        keys[i] = (uint8_t)k;
    }
    if (p->cbs.on_kb_multi) p->cbs.on_kb_multi(op, keys, nargs, p->user);
}

static void cmd_kb_isdown(ferrum_parser_t *p, arg_t *args, uint8_t nargs) {
    if (nargs != 1) return;
    int32_t k;
    if (!parse_int(args[0].p, args[0].len, &k)) return;
    if (p->cbs.on_kb_isdown) p->cbs.on_kb_isdown((uint8_t)k, p->user);
}

static void cmd_kb_mask(ferrum_parser_t *p, arg_t *args, uint8_t nargs) {
    if (nargs == 1) {
        int32_t k;
        if (!parse_int(args[0].p, args[0].len, &k)) return;
        if (p->cbs.on_kb_mask_get) p->cbs.on_kb_mask_get((uint8_t)k, p->user);
        return;
    }
    if (nargs != 2) return;
    int32_t k, s;
    if (!parse_int(args[0].p, args[0].len, &k)) return;
    if (!parse_int(args[1].p, args[1].len, &s)) return;
    if (p->cbs.on_kb_mask_set) p->cbs.on_kb_mask_set((uint8_t)k, (uint8_t)s, p->user);
}

static void cmd_baud(ferrum_parser_t *p, arg_t *args, uint8_t nargs) {
    if (nargs != 1) return;
    int32_t n;
    if (!parse_int(args[0].p, args[0].len, &n)) return;
    if (n <= 0) return;
    if (p->cbs.on_baud) p->cbs.on_baud((uint32_t)n, p->user);
}

static void cmd_cb_toggle(ferrum_parser_t *p,
                          void (*on_set)(uint8_t, void *),
                          void (*on_get)(void *),
                          arg_t *args, uint8_t nargs) {
    if (nargs == 0) {
        if (on_get) on_get(p->user);
        return;
    }
    if (nargs != 1) return;
    int32_t s;
    if (!parse_int(args[0].p, args[0].len, &s)) return;
    if (s != 0 && s != 1) return;
    if (on_set) on_set((uint8_t)s, p->user);
}

static void dispatch(ferrum_parser_t *p, const char *name, uint8_t name_len,
                     arg_t *args, uint8_t nargs) {
    if (name_is(name, name_len, "version")) {
        if (p->cbs.on_version) p->cbs.on_version(p->user); return;
    }
    if (name_is(name, name_len, "move"))      { cmd_move(p, args, nargs); return; }

    if (name_is(name, name_len, "left"))      { cmd_button(p, 0x01, args, nargs); return; }
    if (name_is(name, name_len, "right"))     { cmd_button(p, 0x02, args, nargs); return; }
    if (name_is(name, name_len, "middle"))    { cmd_button(p, 0x04, args, nargs); return; }
    if (name_is(name, name_len, "side1"))     { cmd_button(p, 0x08, args, nargs); return; }
    if (name_is(name, name_len, "side2"))     { cmd_button(p, 0x10, args, nargs); return; }

    if (name_is(name, name_len, "click"))     { cmd_click(p, args, nargs); return; }
    if (name_is(name, name_len, "wheel"))     { cmd_wheel(p, args, nargs); return; }

    if (name_is(name, name_len, "lock_ml"))   { cmd_lock(p, "ml",  args, nargs); return; }
    if (name_is(name, name_len, "lock_mr"))   { cmd_lock(p, "mr",  args, nargs); return; }
    if (name_is(name, name_len, "lock_mm"))   { cmd_lock(p, "mm",  args, nargs); return; }
    if (name_is(name, name_len, "lock_ms1"))  { cmd_lock(p, "ms1", args, nargs); return; }
    if (name_is(name, name_len, "lock_ms2"))  { cmd_lock(p, "ms2", args, nargs); return; }
    if (name_is(name, name_len, "lock_mx"))   { cmd_lock(p, "mx",  args, nargs); return; }
    if (name_is(name, name_len, "lock_my"))   { cmd_lock(p, "my",  args, nargs); return; }

    if (name_is(name, name_len, "catch_xy"))  { cmd_catch_xy(p, args, nargs); return; }

    if (name_is(name, name_len, "down"))      { cmd_kb_simple(p, args, nargs, 0); return; }
    if (name_is(name, name_len, "up"))        { cmd_kb_simple(p, args, nargs, 1); return; }
    if (name_is(name, name_len, "press"))     { cmd_kb_simple(p, args, nargs, 2); return; }
    if (name_is(name, name_len, "multidown")) { cmd_kb_multi(p, args, nargs, 0); return; }
    if (name_is(name, name_len, "multiup"))   { cmd_kb_multi(p, args, nargs, 1); return; }
    if (name_is(name, name_len, "multipress")){ cmd_kb_multi(p, args, nargs, 2); return; }
    if (name_is(name, name_len, "isdown"))    { cmd_kb_isdown(p, args, nargs); return; }
    if (name_is(name, name_len, "mask"))      { cmd_kb_mask(p, args, nargs); return; }

    if (name_is(name, name_len, "init")) {
        if (p->cbs.on_init) p->cbs.on_init(p->user); return;
    }

    if (name_is(name, name_len, "buttons")) {
        cmd_cb_toggle(p, p->cbs.on_cb_buttons_set, p->cbs.on_cb_buttons_get,
                      args, nargs);
        return;
    }
    if (name_is(name, name_len, "axes")) {
        cmd_cb_toggle(p, p->cbs.on_cb_axes_set, p->cbs.on_cb_axes_get,
                      args, nargs);
        return;
    }
    if (name_is(name, name_len, "keys")) {
        cmd_cb_toggle(p, p->cbs.on_cb_keys_set, p->cbs.on_cb_keys_get,
                      args, nargs);
        return;
    }

    if (name_is(name, name_len, "baud"))      { cmd_baud(p, args, nargs); return; }

    /* Unknown → silent drop, matching ferrum.c. */
}

static void dispatch_line(ferrum_parser_t *p, char *line, uint8_t len) {
    uint8_t i = 0;
    while (i < len && (line[i] == ' ' || line[i] == '\t')) i++;
    if (i >= len) return;

    const char *body = NULL;
    uint8_t     body_len = 0;
    const char *name = NULL;
    uint8_t     name_len = 0;

    if (line[i] == 'm' && i + 1 < len && line[i + 1] == '(') {
        name = "move"; name_len = 4;
        body = &line[i + 2];
        body_len = (uint8_t)(len - (i + 2));
    } else if (i + 3 <= len && line[i] == 'k' && line[i + 1] == 'm' &&
               line[i + 2] == '.') {
        uint8_t name_start = i + 3;
        uint8_t q = name_start;
        while (q < len && line[q] != '(') q++;
        if (q >= len) return;
        name = &line[name_start];
        name_len = (uint8_t)(q - name_start);
        body = &line[q + 1];
        body_len = (uint8_t)(len - (q + 1));
    } else {
        return;
    }

    if (name_len == 0) return;

    uint8_t b = body_len;
    while (b > 0 && body[b - 1] != ')') b--;
    if (b == 0) return;
    b--;

    arg_t args[FERRUM_MAX_ARGS];
    uint8_t nargs = split_args(body, b, args);

    dispatch(p, name, name_len, args, nargs);
}

void ferrum_parser_feed_byte(ferrum_parser_t *p, uint8_t b) {
    if (!p) return;
    uint32_t now = now_ms();

    if (p->line_pos > 0 && (now - p->last_byte_ms) > FERRUM_IDLE_GAP_MS) {
        p->line_pos = 0;
        p->overflow = false;
    }
    p->last_byte_ms = now;

    if (b == '\r' || b == '\n') {
        if (p->line_pos > 0 && !p->overflow) {
            p->line[p->line_pos] = '\0';
            dispatch_line(p, p->line, p->line_pos);
        }
        p->line_pos = 0;
        p->overflow = false;
        return;
    }
    if (p->line_pos < FERRUM_LINE_MAX - 1) {
        p->line[p->line_pos++] = (char)b;
    } else {
        p->overflow = true;
    }
}

void ferrum_parser_tick(ferrum_parser_t *p) {
    if (!p || p->line_pos == 0) return;
    uint32_t now = now_ms();
    if ((now - p->last_byte_ms) > FERRUM_IDLE_GAP_MS) {
        p->line_pos = 0;
        p->overflow = false;
    }
}
