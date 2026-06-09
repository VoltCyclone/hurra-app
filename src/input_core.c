#include "input_core.h"
#include "hurra.h"
#include <stdint.h>

static void clamp_i16(int32_t *v) {
    if (*v > INT16_MAX) *v = INT16_MAX;
    if (*v < INT16_MIN) *v = INT16_MIN;
}

static void core_move(input_sink_t *s, int32_t dx, int32_t dy) {
    clamp_i16(&dx); clamp_i16(&dy);
    (void)hurra_move((hurra_client_t *)s->ctx, (int16_t)dx, (int16_t)dy);
    if (s->move_count) (*s->move_count)++;
}
static void core_button(input_sink_t *s, int btn, int down) {
    if (btn < 0 || btn > 4) return;
    (void)hurra_button((hurra_client_t *)s->ctx, (uint8_t)btn, down ? 1 : 0);
}
static void core_wheel(input_sink_t *s, int32_t ticks) {
    if (ticks > INT8_MAX) ticks = INT8_MAX;
    if (ticks < INT8_MIN) ticks = INT8_MIN;
    (void)hurra_wheel((hurra_client_t *)s->ctx, (int8_t)ticks);
}
static void core_mouse_all(input_sink_t *s, uint8_t buttons,
                           int32_t dx, int32_t dy, int32_t wheel) {
    clamp_i16(&dx); clamp_i16(&dy);
    if (wheel > INT8_MAX) wheel = INT8_MAX;
    if (wheel < INT8_MIN) wheel = INT8_MIN;
    (void)hurra_mo((hurra_client_t *)s->ctx, buttons,
                   (int16_t)dx, (int16_t)dy, (int8_t)wheel, 0, 0);
}
static void core_kb_report(input_sink_t *s, uint8_t modifier,
                           const uint8_t *keys, int nkeys) {
    hurra_client_t *hc = (hurra_client_t *)s->ctx;
    /* Modifier byte -> individual modifier HID downs (0xE0..0xE7). */
    for (int b = 0; b < 8; b++) {
        if (modifier & (1u << b)) (void)hurra_kb_down(hc, (uint8_t)(0xE0 + b));
    }
    /* Firmware caps at 6 keys; truncate extras. */
    size_t n = (nkeys > 6) ? 6 : (size_t)(nkeys < 0 ? 0 : nkeys);
    if (n > 0) (void)hurra_kb_multidown(hc, keys, n);
}
static void core_reboot(input_sink_t *s) {
    (void)hurra_reboot((hurra_client_t *)s->ctx);
}

int input_core_init(input_sink_t *out, struct hurra_client *hc) {
    if (!out || !hc) return -1;
    out->ctx        = hc;
    out->move       = core_move;
    out->button     = core_button;
    out->wheel      = core_wheel;
    out->mouse_all  = core_mouse_all;
    out->kb_report  = core_kb_report;
    out->reboot     = core_reboot;
    out->move_count = NULL;
    return 0;
}
