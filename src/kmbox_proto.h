/*
 * kmbox_proto.h — KMBox Net UDP wire constants and header layout.
 *
 * Reference: https://github.com/ZCban/kmboxNET (kmboxNet.h).
 * The bridge emulates the DEVICE side: clients send these packets, the device
 * echoes the 16-byte header as an ACK. All fields little-endian.
 */
#ifndef HURRA_KMBOX_PROTO_H
#define HURRA_KMBOX_PROTO_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Command codes (verbatim from reference SDK). */
#define KM_CMD_CONNECT       0xAF3C2828u
#define KM_CMD_MOUSE_MOVE    0xAEDE7345u
#define KM_CMD_MOUSE_LEFT    0x9823AE8Du
#define KM_CMD_MOUSE_MIDDLE  0x97A3AE8Du
#define KM_CMD_MOUSE_RIGHT   0x238D8212u
#define KM_CMD_MOUSE_WHEEL   0xFFEEAD38u
#define KM_CMD_MOUSE_AUTOMOVE 0xAEDE7346u
#define KM_CMD_BEZIER        0xA238455Au
#define KM_CMD_KEYBOARD_ALL  0x123C2C2Fu
#define KM_CMD_REBOOT        0xAA8855AAu
#define KM_CMD_MONITOR       0x27388020u
#define KM_CMD_MASK_MOUSE    0x23234343u
#define KM_CMD_UNMASK_ALL    0x23344343u

/* 16-byte header: four little-endian uint32. */
#define KM_HEAD_SIZE 16

typedef struct {
    uint32_t mac;
    uint32_t rand;
    uint32_t indexpts;
    uint32_t cmd;
} km_head_t;

/* soft_mouse_t on the wire: int32 button, x, y, wheel, point[10] = 56 bytes.
 * soft_keyboard_t: int8 ctrl, int8 resvel, uint8 button[10] = 12 bytes. */
#define KM_MOUSE_PAYLOAD_SIZE 56
#define KM_MOUSE_CORE_SIZE    16  /* button,x,y,wheel: first 4 int32 the bridge uses */
#define KM_KB_PAYLOAD_SIZE    12
#define KM_KB_MAX_KEYS        10

#ifdef __cplusplus
}
#endif

#endif /* HURRA_KMBOX_PROTO_H */
