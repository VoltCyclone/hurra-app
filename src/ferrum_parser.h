/*
 * ferrum_parser.h — host-side port of the iMXRT Ferrum ASCII parser.
 *
 * Identical wire surface to imxrtnsy/src/ferrum.c:
 *   km.<name>(<args>)\r\n   or   m(x, y)\r\n
 *   replies are emitted on the same channel as bare text per ferrum
 *   conventions (e.g. "0\r\n", "1\r\n", "(x, y)\r\n").
 *
 * The action layer (act_*, kmbox_inject_*) is replaced by a callback
 * table. Each callback fires synchronously from ferrum_parser_feed_byte()
 * when a complete command line is dispatched.
 *
 * The parser does NOT emit reply text itself for get-style commands —
 * its caller (bridge.c) issues the corresponding hurra_* request and
 * writes the formatted reply. Helper formatters are also exposed so the
 * bridge can produce ferrum-compatible text.
 */
#ifndef HURRA_FERRUM_PARSER_H
#define HURRA_FERRUM_PARSER_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct ferrum_parser ferrum_parser_t;

typedef struct {
    /* Mouse / movement */
    void (*on_version)        (void *user);
    void (*on_move)           (int32_t x, int32_t y, void *user);
    void (*on_button_set)     (uint8_t mask, uint8_t state, void *user);
    void (*on_button_get)     (uint8_t mask, void *user);
    void (*on_click)          (uint8_t button_0based, void *user);
    void (*on_wheel)          (int32_t n, void *user);

    /* Locks */
    void (*on_lock_set)       (const char *name, uint8_t state, void *user);
    void (*on_lock_get)       (const char *name, void *user);

    /* catch_xy */
    void (*on_catch_xy)       (uint32_t dur_ms, void *user);

    /* Keyboard */
    void (*on_kb_down)        (uint8_t hid, void *user);
    void (*on_kb_up)          (uint8_t hid, void *user);
    void (*on_kb_press)       (uint8_t hid, void *user);
    /* op: 0=down, 1=up, 2=press */
    void (*on_kb_multi)       (int op, const uint8_t *keys, size_t n, void *user);
    void (*on_kb_isdown)      (uint8_t hid, void *user);
    void (*on_kb_mask_set)    (uint8_t hid, uint8_t state, void *user);
    void (*on_kb_mask_get)    (uint8_t hid, void *user);

    /* Init / baud */
    void (*on_init)           (void *user);
    void (*on_baud)           (uint32_t baud, void *user);

    /* Callback toggles */
    void (*on_cb_buttons_set) (uint8_t enable, void *user);
    void (*on_cb_buttons_get) (void *user);
    void (*on_cb_axes_set)    (uint8_t enable, void *user);
    void (*on_cb_axes_get)    (void *user);
    void (*on_cb_keys_set)    (uint8_t enable, void *user);
    void (*on_cb_keys_get)    (void *user);
} ferrum_callbacks_t;

ferrum_parser_t *ferrum_parser_create(const ferrum_callbacks_t *cbs, void *user);
void             ferrum_parser_destroy(ferrum_parser_t *p);

/* Feed one byte of input. Triggers callbacks on terminator (\r or \n). */
void ferrum_parser_feed_byte(ferrum_parser_t *p, uint8_t b);

/* Idle-gap timer tick. Drops any partial line if no byte has arrived in
 * the last FERRUM_IDLE_GAP_MS milliseconds. Bridge calls this periodically
 * from the main loop. */
void ferrum_parser_tick(ferrum_parser_t *p);

/* ── Ferrum-compatible text emitters (bridge uses these for replies) ───── */

/* Format a signed int into buf. Returns number of chars written (no NUL).
 * Caller must guarantee buf has >= 12 bytes. */
size_t ferrum_format_int(int32_t v, char *buf);

/* "kmbox: Ferrum\r\n" length-prefixed write helper. Pass a writer that
 * accepts (buf, n). */
typedef void (*ferrum_write_fn)(const uint8_t *buf, size_t n, void *user);
void ferrum_emit_version_text (ferrum_write_fn w, void *u);
void ferrum_emit_bool         (ferrum_write_fn w, void *u, int v);
void ferrum_emit_pair         (ferrum_write_fn w, void *u, int32_t x, int32_t y);
/* "km.<bitmap_byte>\r\n" — single raw byte after "km.". */
void ferrum_emit_buttons_cb   (ferrum_write_fn w, void *u, uint8_t bitmap);
void ferrum_emit_axes_cb      (ferrum_write_fn w, void *u,
                               int32_t dx, int32_t dy, int32_t scroll);
/* keys[6]; the helper sorts ascending and skips zero slots. */
void ferrum_emit_keys_cb      (ferrum_write_fn w, void *u, const uint8_t keys[6]);

#ifdef __cplusplus
}
#endif

#endif /* HURRA_FERRUM_PARSER_H */
