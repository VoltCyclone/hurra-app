/*
 * hurra.h — public C API for the Hurra protocol host adapter.
 *
 * Pairs with the iMXRT firmware in https://github.com/ramseymcgrath/imxrtnsy
 * (see docs/specs/2026-05-23-hurra-binary-protocol-design.md).
 *
 * Design notes
 *   * Opaque-handle API. One hurra_client_t per serial port.
 *   * The library does NOT spawn its own threads. Call hurra_poll() in your
 *     main loop frequently (≥1 kHz at 4 Mbps) to drain RX and dispatch
 *     telemetry/reply callbacks.
 *   * Hot-path commands (move/wheel/click/button) are oneway — no reply, no
 *     ACK. They return 0 on serial write success, -1 on write error.
 *   * Request/reply helpers (version/ping/stats/getpos/get_baud/screen_get/
 *     kb_isdown/lock-get) block up to timeout_ms for the matching reply.
 *   * Reply correlation is by TinyFrame ID. Up to TF_MAX_ID_LST (8) requests
 *     may be in-flight simultaneously.
 */
#ifndef HURRA_H
#define HURRA_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "hurra_types.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct hurra_client hurra_client_t;

/* Telemetry callback. `type` is a HURRA_TYPE_TLM_* value, `data` is the raw
 * little-endian payload exactly as it appeared on the wire. The buffer is
 * borrowed; copy if you need it past the callback return.
 */
typedef void (*hurra_telemetry_cb)(uint8_t type, const uint8_t *data,
                                   uint16_t len, void *user);

/* ── Lifecycle ───────────────────────────────────────────────────────────── */

/* Open the serial port and configure baud (4_000_000 for stock firmware).
 * Returns NULL on failure; check errno or stderr for cause.
 */
hurra_client_t *hurra_open(const char *port, uint32_t baud);
void            hurra_close(hurra_client_t *c);

/* Drain incoming bytes and dispatch telemetry/reply callbacks. Call this
 * frequently in your main loop. Returns bytes drained, or -1 on serial error.
 */
int hurra_poll(hurra_client_t *c);

/* TX batching for USB-UART throughput.
 *
 * CH343B (and most full-speed CDC-ACM bridges) ships one USB bulk packet
 * per write() syscall, with a 1 ms USB frame interval and 64-byte MPS. A
 * naive "one km.move per write" loop is capped at ~1 kHz no matter the
 * UART baud rate. Batching multiple TinyFrame frames into a single write()
 * before flushing fills the 64-byte packet and pushes throughput up by
 * ~7x for 9-byte move frames.
 *
 * batch_bytes = 0  → immediate flush after every TF frame (default; lowest
 *                    latency, lowest throughput; matches pre-batching v1).
 * batch_bytes > 0  → accumulate up to N bytes (typically 64), flush when the
 *                    next frame would overflow N, when hurra_flush() is
 *                    called, or before any request that waits for a reply.
 *
 * Set this before any hot-path sends. Re-setting it mid-stream flushes the
 * existing buffer.
 */
void hurra_set_tx_batch(hurra_client_t *c, size_t batch_bytes);

/* Flush any pending batched TX. Idempotent. Returns 0 on success, -1 on
 * serial write error. */
int  hurra_flush(hurra_client_t *c);

/* ── Hot path (oneway). Returns 0 on success, -1 on serial write error. ──── */

int hurra_move        (hurra_client_t *c, int16_t dx, int16_t dy);
int hurra_move_smooth (hurra_client_t *c, int16_t dx, int16_t dy);
int hurra_silent_move (hurra_client_t *c, int16_t dx, int16_t dy);
int hurra_mo          (hurra_client_t *c, uint8_t buttons,
                       int16_t dx, int16_t dy,
                       int8_t wheel, int8_t pan, int8_t tilt);
int hurra_click       (hurra_client_t *c, uint8_t button, uint8_t count,
                       uint8_t delay_ms);
int hurra_wheel       (hurra_client_t *c, int8_t ticks);
int hurra_button      (hurra_client_t *c, uint8_t mask, uint8_t state);

/* ── Request/reply ───────────────────────────────────────────────────────── */

int hurra_version (hurra_client_t *c, char *out, size_t outsz, int timeout_ms);
int hurra_ping    (hurra_client_t *c, uint64_t *rtt_us, int timeout_ms);
int hurra_stats   (hurra_client_t *c, hurra_stats_t *out, int timeout_ms);
int hurra_getpos  (hurra_client_t *c, int16_t *x, int16_t *y, int timeout_ms);
int hurra_get_baud(hurra_client_t *c, uint32_t *baud, int timeout_ms);
int hurra_set_baud(hurra_client_t *c, uint32_t baud, int timeout_ms);

/* ── Admin (oneway except where noted) ───────────────────────────────────── */

int hurra_init_remote(hurra_client_t *c);   /* TYPE_INIT  — clears state */
int hurra_reboot     (hurra_client_t *c);   /* TYPE_REBOOT — SYSRESETREQ */
int hurra_screen_set (hurra_client_t *c, int16_t w, int16_t h);
int hurra_screen_get (hurra_client_t *c, int16_t *w, int16_t *h, int timeout_ms);

/* ── Keyboard ────────────────────────────────────────────────────────────── */

int hurra_kb_down       (hurra_client_t *c, uint8_t hid);
int hurra_kb_up         (hurra_client_t *c, uint8_t hid);
int hurra_kb_press      (hurra_client_t *c, uint8_t hid,
                         uint8_t hold_ms, uint8_t rand_ms);
int hurra_kb_isdown     (hurra_client_t *c, uint8_t hid, bool *out,
                         int timeout_ms);
int hurra_kb_mask       (hurra_client_t *c, uint8_t hid, uint8_t state);
int hurra_kb_string     (hurra_client_t *c, const char *s);
int hurra_kb_multidown  (hurra_client_t *c, const uint8_t *keys, size_t n);
int hurra_kb_multiup    (hurra_client_t *c, const uint8_t *keys, size_t n);
int hurra_kb_multipress (hurra_client_t *c, const uint8_t *keys, size_t n);

/* ── Locks ───────────────────────────────────────────────────────────────── */

/* `name` ∈ {"ml","mr","mm","ms1","ms2","mx","my"}.
 * If state_inout is NULL → unsupported (always pass a pointer; behavior is
 *   set when *state_inout ≥ 0, get when *state_inout < 0).
 * On get, *state_inout is overwritten with the firmware-reported value.
 */
int hurra_lock(hurra_client_t *c, const char *name, int *state_inout,
               int timeout_ms);

/* catch_xy: blocks for dur_ms + 1000 ms waiting for the deferred reply. */
int hurra_catch_xy(hurra_client_t *c, uint16_t dur_ms,
                   int32_t *dx_accum, int32_t *dy_accum);

/* ── Telemetry streams ───────────────────────────────────────────────────── */

int hurra_stream_axis     (hurra_client_t *c, uint8_t mode, uint8_t period_ms);
int hurra_stream_buttons  (hurra_client_t *c, uint8_t mode, uint8_t period_ms);
int hurra_stream_mouse    (hurra_client_t *c, uint8_t mode, uint8_t period_ms);
int hurra_stream_keyboard (hurra_client_t *c, uint8_t mode, uint8_t period_ms);

/* ── Change-only callback toggles ────────────────────────────────────────── */

/* Send a single-byte enable payload to CB_BUTTONS / CB_AXES / CB_KEYS. The
 * firmware emits a TLM_* frame only when the corresponding state changes.
 * Oneway; returns 0 on success, -1 on serial write error.
 */
int hurra_cb_buttons (hurra_client_t *c, uint8_t enable);
int hurra_cb_axes    (hurra_client_t *c, uint8_t enable);
int hurra_cb_keys    (hurra_client_t *c, uint8_t enable);

/* Subscribe a callback for a TLM_* type. One callback per type slot.
 * Passing handler=NULL unsubscribes.
 */
int hurra_on_telemetry(hurra_client_t *c, uint8_t type,
                       hurra_telemetry_cb handler, void *user);

#ifdef __cplusplus
}
#endif

#endif /* HURRA_H */
