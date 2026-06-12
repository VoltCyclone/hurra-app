/* ferrum_framing_test — byte-exact Software-API framing per ferrumSpec.md §2/§3/§5/§6/§7/§9.
 *   cc -std=c99 -Wall -Wextra -Isrc -o build/ferrum_framing_test \
 *      tests/ferrum_framing_test.c src/ferrum_parser.c
 * Drives ferrum_parser with a capturing writer and asserts the exact bytes a
 * Ferrum/KMBox/MAKCU client would see, including the NUL byte in the button
 * callback. No hurra link — the parser is self-contained.
 */
#include "ferrum_parser.h"
#include <stdio.h>
#include <stdint.h>
#include <string.h>

static int g_fail = 0;

/* ── Capture sink ──────────────────────────────────────────────────────── */
static uint8_t  g_buf[4096];
static size_t   g_len;
static void cap_write(const uint8_t *buf, size_t n, void *user) {
    (void)user;
    if (g_len + n > sizeof(g_buf)) n = sizeof(g_buf) - g_len;
    memcpy(g_buf + g_len, buf, n);
    g_len += n;
}
static void cap_reset(void) { g_len = 0; memset(g_buf, 0, sizeof(g_buf)); }

/* Compare captured bytes against an expected buffer of known length (NUL-safe). */
static void expect_bytes(const char *label, const uint8_t *exp, size_t exp_len) {
    int ok = (g_len == exp_len) && (memcmp(g_buf, exp, exp_len) == 0);
    if (!ok) {
        g_fail = 1;
        printf("FAIL %s\n  want (%zu): ", label, exp_len);
        for (size_t i = 0; i < exp_len; i++) printf("%02x ", exp[i]);
        printf("\n  got  (%zu): ", g_len);
        for (size_t i = 0; i < g_len; i++) printf("%02x ", g_buf[i]);
        printf("\n");
    } else {
        printf("ok   %s\n", label);
    }
}
static void expect_str(const char *label, const char *exp) {
    expect_bytes(label, (const uint8_t *)exp, strlen(exp));
}

/* ── Callbacks: enough to exercise version, a get, a no-return, an unknown ─ */
static void cb_version(void *u) {
    /* version text is emitted by the bridge/frontend layer in production; here
     * we mimic that by writing it through the same capture writer. */
    ferrum_emit_version_text(cap_write, u);
}
static int g_last_move_x, g_last_move_y, g_moves;
static void cb_move(int32_t x, int32_t y, void *u){ (void)u; g_moves++; g_last_move_x=x; g_last_move_y=y; }
static void cb_button_get(uint8_t mask, void *u){ (void)mask; ferrum_emit_bool(cap_write, u, 1); }
static int g_catch_called; static uint32_t g_catch_dur;
static void cb_catch(uint32_t dur, void *u){ (void)u; g_catch_called++; g_catch_dur=dur; }

static ferrum_parser_t *make_parser(void) {
    ferrum_callbacks_t cbs; memset(&cbs, 0, sizeof cbs);
    cbs.on_version    = cb_version;
    cbs.on_move       = cb_move;
    cbs.on_button_get = cb_button_get;
    cbs.on_catch_xy   = cb_catch;
    ferrum_parser_t *p = ferrum_parser_create(&cbs, NULL);
    ferrum_parser_set_writer(p, cap_write, NULL);
    return p;
}

static void feed(ferrum_parser_t *p, const char *s) {
    for (const char *c = s; *c; c++) ferrum_parser_feed_byte(p, (uint8_t)*c);
}

/* ── Tests ─────────────────────────────────────────────────────────────── */

/* §3 + §2: km.version() → echo + value + prompt, terminator normalized. */
static void test_version_framing(void) {
    ferrum_parser_t *p = make_parser();
    cap_reset();
    feed(p, "km.version()\n");          /* client sends bare \n */
    expect_str("version: echo+value+prompt, \\n normalized to \\r\\n",
               "km.version()\r\n" "kmbox: Ferrum\r\n" ">>> ");
    ferrum_parser_destroy(p);
}

/* §2: no-return command → echo + prompt, no value between. */
static void test_noreturn_framing(void) {
    ferrum_parser_t *p = make_parser();
    cap_reset();
    feed(p, "km.move(10,   10)\r\n");   /* arbitrary inner whitespace */
    expect_str("move: echo+prompt only (no value)",
               "km.move(10,   10)\r\n" ">>> ");
    if (!(g_moves==1 && g_last_move_x==10 && g_last_move_y==10)) {
        g_fail=1; printf("FAIL move parse x=%d y=%d moves=%d\n", g_last_move_x,g_last_move_y,g_moves);
    } else printf("ok   move parsed (10,10) through whitespace\n");
    ferrum_parser_destroy(p);
}

/* §2: get command → echo + "1\r\n" value + prompt, in order. */
static void test_get_framing(void) {
    ferrum_parser_t *p = make_parser();
    cap_reset();
    feed(p, "km.left()\r\n");
    expect_str("get: echo + value + prompt ordered",
               "km.left()\r\n" "1\r\n" ">>> ");
    ferrum_parser_destroy(p);
}

/* §5: m(x,y) alias echoes VERBATIM as "m(...)", not "km.move(...)". */
static void test_alias_verbatim_echo(void) {
    ferrum_parser_t *p = make_parser();
    cap_reset();
    feed(p, "m(5,10)\r\n");
    expect_str("alias m(): verbatim echo + prompt",
               "m(5,10)\r\n" ">>> ");
    ferrum_parser_destroy(p);
}

/* §2: bare extra \n / run of terminators must NOT emit an empty response. */
static void test_blank_line_silent(void) {
    ferrum_parser_t *p = make_parser();
    cap_reset();
    feed(p, "\r\n\n\r");               /* nothing but terminators */
    expect_bytes("blank lines: no output", (const uint8_t *)"", 0);
    /* And a trailing terminator after a real command produces exactly one frame. */
    cap_reset();
    feed(p, "km.left()\r\n\n");        /* extra \n collapses */
    expect_str("trailing extra \\n: single frame",
               "km.left()\r\n" "1\r\n" ">>> ");
    ferrum_parser_destroy(p);
}

/* Unknown command → still echo + prompt so the client's >>> sync survives. */
static void test_unknown_keeps_sync(void) {
    ferrum_parser_t *p = make_parser();
    cap_reset();
    feed(p, "km.bogus(1)\r\n");
    expect_str("unknown: echo + prompt (no value), sync preserved",
               "km.bogus(1)\r\n" ">>> ");
    ferrum_parser_destroy(p);
}

/* §6: button callback = "km." + raw bitmap byte + "\r\n>>> "; NUL when empty. */
static void test_button_cb_raw_byte(void) {
    cap_reset();
    ferrum_emit_buttons_cb(cap_write, NULL, 0x09);   /* left+side1 */
    { const uint8_t exp[] = {'k','m','.',0x09,'\r','\n','>','>','>',' '};
      expect_bytes("button cb: km.<0x09>\\r\\n>>> ", exp, sizeof exp); }
    cap_reset();
    ferrum_emit_buttons_cb(cap_write, NULL, 0x00);   /* none held → NUL */
    { const uint8_t exp[] = {'k','m','.',0x00,'\r','\n','>','>','>',' '};
      expect_bytes("button cb: NUL byte preserved", exp, sizeof exp); }
}

/* §7: axis callback exact "Axes(x, y, scroll)\r\n>>> ". */
static void test_axes_cb(void) {
    cap_reset();
    ferrum_emit_axes_cb(cap_write, NULL, 100, -50, 1);
    expect_str("axes cb format", "Axes(100, -50, 1)\r\n>>> ");
}

/* §9: key callback decimal, ascending, "Keys()" empty, "\r\n>>> " suffix. */
static void test_keys_cb(void) {
    cap_reset();
    { uint8_t keys[6] = {14, 4, 0, 0, 0, 0};        /* unsorted */
      ferrum_emit_keys_cb(cap_write, NULL, keys); }
    expect_str("keys cb: ascending sort", "Keys(4, 14)\r\n>>> ");
    cap_reset();
    { uint8_t keys[6] = {0};
      ferrum_emit_keys_cb(cap_write, NULL, keys); }
    expect_str("keys cb: empty", "Keys()\r\n>>> ");
}

/* §5: catch_xy include_sw bool accepted in ANY case. */
static void test_catch_xy_bool_case(void) {
    ferrum_parser_t *p = make_parser();
    g_catch_called = 0;
    feed(p, "km.catch_xy(50, TRUE)\r\n");
    if (g_catch_called == 1 && g_catch_dur == 50)
        printf("ok   catch_xy(50, TRUE) accepted (case-insensitive)\n");
    else { g_fail=1; printf("FAIL catch_xy uppercase bool dropped (called=%d)\n", g_catch_called); }
    g_catch_called = 0;
    feed(p, "km.catch_xy(25, False)\r\n");
    if (g_catch_called == 1 && g_catch_dur == 25)
        printf("ok   catch_xy(25, False) accepted (mixed case)\n");
    else { g_fail=1; printf("FAIL catch_xy mixed-case bool dropped (called=%d)\n", g_catch_called); }
    ferrum_parser_destroy(p);
}

/* No-writer mode (Legacy API / default) emits NO framing — only callback values. */
static void test_no_writer_legacy(void) {
    ferrum_callbacks_t cbs; memset(&cbs, 0, sizeof cbs);
    cbs.on_button_get = cb_button_get;
    ferrum_parser_t *p = ferrum_parser_create(&cbs, NULL);
    /* deliberately NO set_writer */
    cap_reset();
    feed(p, "km.left()\r\n");
    /* Only the value the callback itself emits — no echo, no prompt. */
    expect_str("no-writer: value only, no echo/prompt", "1\r\n");
    ferrum_parser_destroy(p);
}

int main(void) {
    test_version_framing();
    test_noreturn_framing();
    test_get_framing();
    test_alias_verbatim_echo();
    test_blank_line_silent();
    test_unknown_keeps_sync();
    test_button_cb_raw_byte();
    test_axes_cb();
    test_keys_cb();
    test_catch_xy_bool_case();
    test_no_writer_legacy();
    if (g_fail) { printf("\nFAILED\n"); return 1; }
    printf("\nAll framing tests passed\n");
    return 0;
}
