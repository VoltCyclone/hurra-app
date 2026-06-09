/*
 * input_core.h — abstract input events + a hurra-backed sink.
 *
 * Frontends (vcom, kmbox) translate their protocol into these calls; the
 * concrete sink forwards them to libhurra. The sink holds NO protocol state.
 */
#ifndef HURRA_INPUT_CORE_H
#define HURRA_INPUT_CORE_H

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Abstract input sink. Every frontend drives one of these. */
typedef struct input_sink {
    void *ctx;
    void (*move)      (struct input_sink *s, int32_t dx, int32_t dy);
    /* btn: 0=L,1=R,2=M,3=S1,4=S2 ; down=0/1 */
    void (*button)    (struct input_sink *s, int btn, int down);
    void (*wheel)     (struct input_sink *s, int32_t ticks);
    /* Combined report: button bitmask + deltas + wheel. */
    void (*mouse_all) (struct input_sink *s, uint8_t buttons,
                       int32_t dx, int32_t dy, int32_t wheel);
    /* Full keyboard report: modifier byte + up to nkeys HID codes held. */
    void (*kb_report) (struct input_sink *s, uint8_t modifier,
                       const uint8_t *keys, int nkeys);
    void (*reboot)    (struct input_sink *s);
    /* Optional: if non-NULL, core_move increments *move_count per move.
     * Lets the host track a moves metric without the sink knowing about it. */
    uint64_t *move_count;
} input_sink_t;

/* Construct a sink that forwards to a hurra_client_t. `hc` is borrowed.
 * Returns 0 on success and fills *out. */
struct hurra_client;  /* fwd decl; real type from hurra.h */
int  input_core_init(input_sink_t *out, struct hurra_client *hc);

#ifdef __cplusplus
}
#endif

#endif /* HURRA_INPUT_CORE_H */
