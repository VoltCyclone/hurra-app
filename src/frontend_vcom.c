/*
 * frontend_vcom.c — VCOM (PTY/com0com) frontend: Ferrum ASCII over virtual port.
 *
 * Extracted from bridge.c: all ferrum callbacks, telemetry handlers, and the
 * vp_write_all/writer_for_emit adapter, now wired to an input_sink_t for the
 * move/button/wheel hot path and calling hurra directly for everything else.
 *
 * Behaviour is intentionally byte-identical to the original bridge.c code;
 * only the context struct (bridge_t → vcom_t) and two UI helpers (status_clear,
 * blog → fprintf(stderr,...)) differ.
 *
 * Intentionally dropped from this frontend (bridge-level concerns):
 *   - __diag__ side-channel  (bridge stats not available here)
 *   - probe_calls/probe_ok/probe_fail counters (bridge-level stats)
 *   - ferrum_lines_in / ferrum_moves / hurra_rx_bytes counters (bridge stats)
 * All of the above remain in bridge.c.
 */

#include "frontend_vcom.h"
#include "ferrum_parser.h"
#include "virtual_port.h"
#include "hurra.h"
#include "hurra_types.h"

#include <errno.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ── VCOM implementation state ─────────────────────────────────────────── */

typedef struct {
    vp_port_t       *vp;
    ferrum_parser_t *parser;
    input_sink_t    *sink;
    hurra_client_t  *hc;
    int              timeout_ms;

    /* Optional host health hook, fired on each `version` re-probe. */
    vcom_health_cb   health_cb;
    void            *health_user;

    /* Per-callback local enable state (ferrum semantics: readable). */
    bool cb_buttons_enabled;
    bool cb_axes_enabled;
    bool cb_keys_enabled;
} vcom_t;

/* ── Write helpers ─────────────────────────────────────────────────────── */

/* Loop-write; drop remainder on buffer-full rather than spinning forever. */
static void vcom_write_all(vp_port_t *vp, const uint8_t *buf, size_t n) {
    size_t off = 0;
    while (off < n) {
        int w = vp_write(vp, buf + off, n - off);
        if (w < 0) return;
        if (w == 0) return;  /* Buffer full / no reader — drop the rest. */
        off += (size_t)w;
    }
}

/* Writer adapter for the ferrum_emit_* helpers. */
static void vcom_write(const uint8_t *buf, size_t n, void *user) {
    vcom_t *v = (vcom_t *)user;
    vcom_write_all(v->vp, buf, n);
}

/* ── Helper: map ferrum button-mask bit → hurra button index ───────────── */

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

/* ── Telemetry handlers (Hurra → Ferrum text) ──────────────────────────── */

static void fv_tlm_buttons(uint8_t type, const uint8_t *data,
                           uint16_t len, void *user) {
    (void)type;
    vcom_t *v = (vcom_t *)user;
    if (!v->cb_buttons_enabled || len < 1) return;
    ferrum_emit_buttons_cb(vcom_write, v, data[0]);
}

static void fv_tlm_axis(uint8_t type, const uint8_t *data,
                        uint16_t len, void *user) {
    (void)type;
    vcom_t *v = (vcom_t *)user;
    if (!v->cb_axes_enabled || len < 5) return;
    /* TLM_AXIS payload: int16 dx | int16 dy | int8 scroll, little-endian. */
    int16_t dx = (int16_t)((uint16_t)data[0] | ((uint16_t)data[1] << 8));
    int16_t dy = (int16_t)((uint16_t)data[2] | ((uint16_t)data[3] << 8));
    int8_t  sc = (int8_t)data[4];
    ferrum_emit_axes_cb(vcom_write, v, dx, dy, sc);
}

static void fv_tlm_kb(uint8_t type, const uint8_t *data,
                      uint16_t len, void *user) {
    (void)type;
    vcom_t *v = (vcom_t *)user;
    if (!v->cb_keys_enabled) return;
    uint8_t keys[6] = {0};
    size_t n = len < 6 ? len : 6;
    memcpy(keys, data, n);
    ferrum_emit_keys_cb(vcom_write, v, keys);
}

/* ── Ferrum callbacks → sink / hurra calls ─────────────────────────────── */

/* Version: probe firmware and emit canonical Ferrum version text.
 * status_clear() is dropped (bridge-only UI); blog() → fprintf(stderr,...).
 * The probe result is reported to the host via health_cb so link health stays
 * fresh; the probe_* counters themselves remain bridge-level state. */
static void fv_version(void *user) {
    vcom_t *v = (vcom_t *)user;
    char tmp[64] = {0};
    int rc = hurra_version(v->hc, tmp, sizeof(tmp), 250);
    if (rc == 0) {
        fprintf(stderr, "version probe ok: fw=\"%s\"\n", tmp);
    } else {
        fprintf(stderr, "version probe FAILED: rc=%d  (firmware not responding on real UART)\n", rc);
    }
    fflush(stderr);
    if (v->health_cb) v->health_cb(v->health_user, rc == 0);
    ferrum_emit_version_text(vcom_write, v);
}

/* Move: route through sink (sink handles clamping). */
static void fv_move(int32_t x, int32_t y, void *user) {
    vcom_t *v = (vcom_t *)user;
    v->sink->move(v->sink, x, y);
}

/* Button set: map ferrum mask bit → index, drive sink. */
static void fv_button_set(uint8_t mask, uint8_t state, void *user) {
    vcom_t *v = (vcom_t *)user;
    int idx = btn_index_for_mask(mask);
    if (idx < 0) return;
    v->sink->button(v->sink, idx, state);
}

static void fv_button_get(uint8_t mask, void *user) {
    vcom_t *v = (vcom_t *)user;
    int idx = btn_index_for_mask(mask);
    if (idx < 0) { ferrum_emit_bool(vcom_write, v, 0); return; }
    bool down = false;
    int rc = hurra_button_get(v->hc, (uint8_t)idx, &down, v->timeout_ms);
    ferrum_emit_bool(vcom_write, v, (rc == 0 && down) ? 1 : 0);
}

/* Click: no sink verb for click; call hurra directly (1-based button). */
static void fv_click(uint8_t button_0based, void *user) {
    vcom_t *v = (vcom_t *)user;
    (void)hurra_click(v->hc, (uint8_t)(button_0based + 1), 1, 0);
}

/* Wheel: route through sink. */
static void fv_wheel(int32_t n, void *user) {
    vcom_t *v = (vcom_t *)user;
    v->sink->wheel(v->sink, n);
}

static void fv_lock_set(const char *name, uint8_t state, void *user) {
    vcom_t *v = (vcom_t *)user;
    int s = state ? 1 : 0;
    (void)hurra_lock(v->hc, name, &s, v->timeout_ms);
}

static void fv_lock_get(const char *name, void *user) {
    vcom_t *v = (vcom_t *)user;
    int s = -1;
    int rc = hurra_lock(v->hc, name, &s, v->timeout_ms);
    ferrum_emit_bool(vcom_write, v, (rc == 0 && s) ? 1 : 0);
}

static void fv_catch_xy(uint32_t dur_ms, void *user) {
    vcom_t *v = (vcom_t *)user;
    if (dur_ms > 1000) dur_ms = 1000;
    int32_t dx = 0, dy = 0;
    int rc = hurra_catch_xy(v->hc, (uint16_t)dur_ms, &dx, &dy);
    if (rc != 0) { dx = 0; dy = 0; }
    ferrum_emit_pair(vcom_write, v, dx, dy);
}

static void fv_kb_down(uint8_t hid, void *user) {
    vcom_t *v = (vcom_t *)user; (void)hurra_kb_down(v->hc, hid);
}
static void fv_kb_up(uint8_t hid, void *user) {
    vcom_t *v = (vcom_t *)user; (void)hurra_kb_up(v->hc, hid);
}
static void fv_kb_press(uint8_t hid, void *user) {
    vcom_t *v = (vcom_t *)user;
    (void)hurra_kb_press(v->hc, hid, 80, 30);
}

static void fv_kb_multi(int op, const uint8_t *keys, size_t n, void *user) {
    vcom_t *v = (vcom_t *)user;
    switch (op) {
        case 0: (void)hurra_kb_multidown (v->hc, keys, n); break;
        case 1: (void)hurra_kb_multiup   (v->hc, keys, n); break;
        case 2: (void)hurra_kb_multipress(v->hc, keys, n); break;
    }
}

static void fv_kb_isdown(uint8_t hid, void *user) {
    vcom_t *v = (vcom_t *)user;
    bool out = false;
    int rc = hurra_kb_isdown(v->hc, hid, &out, v->timeout_ms);
    ferrum_emit_bool(vcom_write, v, (rc == 0 && out) ? 1 : 0);
}

static void fv_kb_mask_set(uint8_t hid, uint8_t state, void *user) {
    vcom_t *v = (vcom_t *)user;
    (void)hurra_kb_mask(v->hc, hid, state);
}

static void fv_kb_mask_get(uint8_t hid, void *user) {
    /* Ferrum returns 0 for the read path (no real getter). */
    (void)hid;
    vcom_t *v = (vcom_t *)user;
    ferrum_emit_bool(vcom_write, v, 0);
}

/* Init: no sink verb; call hurra directly. */
static void fv_init(void *user) {
    vcom_t *v = (vcom_t *)user;
    (void)hurra_init_remote(v->hc);
}

static void fv_baud(uint32_t baud, void *user) {
    vcom_t *v = (vcom_t *)user;
    /* Tells firmware to switch baud; bridge serial link stays at current rate. */
    (void)hurra_set_baud(v->hc, baud, v->timeout_ms);
}

static void fv_human(uint32_t level, void *user) {
    vcom_t *v = (vcom_t *)user;
    (void)hurra_human(v->hc, (uint8_t)level);
}

/* CB toggles: track local enable state and propagate to firmware. */
static void fv_cb_buttons_set(uint8_t enable, void *user) {
    vcom_t *v = (vcom_t *)user;
    v->cb_buttons_enabled = enable != 0;
    (void)hurra_cb_buttons(v->hc, v->cb_buttons_enabled ? 1 : 0);
}
static void fv_cb_buttons_get(void *user) {
    vcom_t *v = (vcom_t *)user;
    ferrum_emit_bool(vcom_write, v, v->cb_buttons_enabled ? 1 : 0);
}
static void fv_cb_axes_set(uint8_t enable, void *user) {
    vcom_t *v = (vcom_t *)user;
    v->cb_axes_enabled = enable != 0;
    (void)hurra_cb_axes(v->hc, v->cb_axes_enabled ? 1 : 0);
}
static void fv_cb_axes_get(void *user) {
    vcom_t *v = (vcom_t *)user;
    ferrum_emit_bool(vcom_write, v, v->cb_axes_enabled ? 1 : 0);
}
static void fv_cb_keys_set(uint8_t enable, void *user) {
    vcom_t *v = (vcom_t *)user;
    v->cb_keys_enabled = enable != 0;
    (void)hurra_cb_keys(v->hc, v->cb_keys_enabled ? 1 : 0);
}
static void fv_cb_keys_get(void *user) {
    vcom_t *v = (vcom_t *)user;
    ferrum_emit_bool(vcom_write, v, v->cb_keys_enabled ? 1 : 0);
}

/* ── frontend_t vtable functions ───────────────────────────────────────── */

static int vcom_poll(frontend_t *fe) {
    vcom_t *v = (vcom_t *)fe->impl;
    uint8_t buf[256];

    int n = vp_read(v->vp, buf, sizeof(buf));
    if (n < 0) return -1;

    for (int i = 0; i < n; i++) {
        uint8_t c = buf[i];
        ferrum_parser_feed_byte(v->parser, c);
    }
    ferrum_parser_tick(v->parser);

    return n;
}

static void vcom_close(frontend_t *fe) {
    vcom_t *v = (vcom_t *)fe->impl;
    if (!v) return;
    ferrum_parser_destroy(v->parser);
    vp_close(v->vp);
    free(v);
    fe->impl = NULL;
}

static const char *vcom_describe(frontend_t *fe) {
    vcom_t *v = (vcom_t *)fe->impl;
    return vp_slave_path(v->vp);
}

/* ── Public API ────────────────────────────────────────────────────────── */

int frontend_vcom_open(frontend_t *out, input_sink_t *sink,
                       struct hurra_client *hc,
                       const char *vp_arg, const char *link_path,
                       int request_timeout_ms,
                       vcom_health_cb health_cb, void *health_user) {
    vcom_t *v = (vcom_t *)calloc(1, sizeof(vcom_t));
    if (!v) return -1;

    v->sink        = sink;
    v->hc          = hc;
    v->timeout_ms  = request_timeout_ms;
    v->health_cb   = health_cb;
    v->health_user = health_user;

    v->vp = vp_open(vp_arg, link_path);
    if (!v->vp) {
        free(v);
        return -1;
    }

    ferrum_callbacks_t cbs;
    memset(&cbs, 0, sizeof(cbs));
    cbs.on_version        = fv_version;
    cbs.on_move           = fv_move;
    cbs.on_button_set     = fv_button_set;
    cbs.on_button_get     = fv_button_get;
    cbs.on_click          = fv_click;
    cbs.on_wheel          = fv_wheel;
    cbs.on_lock_set       = fv_lock_set;
    cbs.on_lock_get       = fv_lock_get;
    cbs.on_catch_xy       = fv_catch_xy;
    cbs.on_kb_down        = fv_kb_down;
    cbs.on_kb_up          = fv_kb_up;
    cbs.on_kb_press       = fv_kb_press;
    cbs.on_kb_multi       = fv_kb_multi;
    cbs.on_kb_isdown      = fv_kb_isdown;
    cbs.on_kb_mask_set    = fv_kb_mask_set;
    cbs.on_kb_mask_get    = fv_kb_mask_get;
    cbs.on_init           = fv_init;
    cbs.on_baud           = fv_baud;
    cbs.on_human          = fv_human;
    cbs.on_cb_buttons_set = fv_cb_buttons_set;
    cbs.on_cb_buttons_get = fv_cb_buttons_get;
    cbs.on_cb_axes_set    = fv_cb_axes_set;
    cbs.on_cb_axes_get    = fv_cb_axes_get;
    cbs.on_cb_keys_set    = fv_cb_keys_set;
    cbs.on_cb_keys_get    = fv_cb_keys_get;

    v->parser = ferrum_parser_create(&cbs, v);
    if (!v->parser) {
        vp_close(v->vp);
        free(v);
        return -1;
    }

    /* Install the Software-API reply channel: verbatim echo + ">>> " prompt
     * around each dispatched line. Same writer the on_* callbacks use, so
     * echo / value / prompt stay strictly ordered on the PTY. */
    ferrum_parser_set_writer(v->parser, vcom_write, v);

    /* Register telemetry handlers. */
    (void)hurra_on_telemetry(hc, HURRA_TYPE_TLM_BUTTONS, fv_tlm_buttons, v);
    (void)hurra_on_telemetry(hc, HURRA_TYPE_TLM_AXIS,    fv_tlm_axis,    v);
    (void)hurra_on_telemetry(hc, HURRA_TYPE_TLM_KB,      fv_tlm_kb,      v);

    out->impl     = v;
    out->poll     = vcom_poll;
    out->close    = vcom_close;
    out->describe = vcom_describe;

    return 0;
}

const char *frontend_vcom_slave_path(frontend_t *fe) {
    vcom_t *v = (vcom_t *)fe->impl;
    return vp_slave_path(v->vp);
}
