/*
 * hurra_types.h — wire-protocol TYPE byte constants + payload structs.
 *
 * These values MUST stay byte-identical with the firmware enum in
 * imxrtnsy/src/hurra.c.  See:
 *   docs/specs/2026-05-23-hurra-binary-protocol-design.md
 *
 * Consumers may include this header without pulling in the full client API
 * (hurra.h); it has no opaque types and no dependencies beyond <stdint.h>.
 */
#ifndef HURRA_TYPES_H
#define HURRA_TYPES_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ── 0x00–0x0F  Admin / diagnostics ──────────────────────────────────────── */
#define HURRA_TYPE_PING               0x00
#define HURRA_TYPE_VERSION            0x01
#define HURRA_TYPE_STATS              0x02
#define HURRA_TYPE_INIT               0x03
#define HURRA_TYPE_REBOOT             0x04
#define HURRA_TYPE_BAUD               0x05
#define HURRA_TYPE_SCREEN             0x06

/* ── 0x10–0x2F  Mouse ────────────────────────────────────────────────────── */
#define HURRA_TYPE_MOUSE_MOVE         0x10
#define HURRA_TYPE_MOUSE_MOVE_SMOOTH  0x11
#define HURRA_TYPE_MOUSE_SILENT_MOVE  0x12
#define HURRA_TYPE_MOUSE_MO           0x13
#define HURRA_TYPE_MOUSE_CLICK        0x14
#define HURRA_TYPE_MOUSE_WHEEL        0x15
#define HURRA_TYPE_MOUSE_GETPOS       0x16
#define HURRA_TYPE_INVERT_X           0x17
#define HURRA_TYPE_INVERT_Y           0x18
#define HURRA_TYPE_SWAP_XY            0x19
#define HURRA_TYPE_HUMAN              0x1A
#define HURRA_TYPE_MOUSE_MOVE_DUR     0x1B  /* int16 dx,dy + uint16 dur_ms (firmware-pending) */
#define HURRA_TYPE_MOUSE_MOVE_BEZIER  0x1C  /* int16 dx,dy,uint16 dur_ms,int16 x1,y1,x2,y2 (firmware-pending) */
#define HURRA_TYPE_BTN_LEFT           0x20
#define HURRA_TYPE_BTN_RIGHT          0x21
#define HURRA_TYPE_BTN_MIDDLE         0x22
#define HURRA_TYPE_BTN_SIDE1          0x23
#define HURRA_TYPE_BTN_SIDE2          0x24

/* ── 0x40–0x4F  Keyboard ─────────────────────────────────────────────────── */
#define HURRA_TYPE_KB_DOWN            0x40
#define HURRA_TYPE_KB_UP              0x41
#define HURRA_TYPE_KB_PRESS           0x42
#define HURRA_TYPE_KB_ISDOWN          0x43
#define HURRA_TYPE_KB_MASK            0x44
#define HURRA_TYPE_KB_STRING          0x45
#define HURRA_TYPE_KB_MULTIDOWN       0x46
#define HURRA_TYPE_KB_MULTIUP         0x47
#define HURRA_TYPE_KB_MULTIPRESS      0x48

/* ── 0x60–0x6F  Locks + catch_xy ─────────────────────────────────────────── */
#define HURRA_TYPE_LOCK_ML            0x60
#define HURRA_TYPE_LOCK_MR            0x61
#define HURRA_TYPE_LOCK_MM            0x62
#define HURRA_TYPE_LOCK_MS1           0x63
#define HURRA_TYPE_LOCK_MS2           0x64
#define HURRA_TYPE_LOCK_MX            0x65
#define HURRA_TYPE_LOCK_MY            0x66
#define HURRA_TYPE_CATCH_XY           0x67
#define HURRA_TYPE_PHYS_MASK          0x68  /* uint8 domain,code,enable — enforce physical mask (firmware-pending) */

/* ── 0x70–0x7F  Stream/callback enable ───────────────────────────────────── */
#define HURRA_TYPE_STREAM_AXIS        0x70
#define HURRA_TYPE_STREAM_BTN         0x71
#define HURRA_TYPE_STREAM_MOUSE       0x72
#define HURRA_TYPE_STREAM_KB          0x73
#define HURRA_TYPE_CB_BUTTONS         0x74
#define HURRA_TYPE_CB_AXES            0x75
#define HURRA_TYPE_CB_KEYS            0x76
#define HURRA_TYPE_CB_PHYS            0x77  /* enable physical-only telemetry (firmware-pending) */

/* ── 0x80–0x8F  Unsolicited firmware-emitted telemetry ───────────────────── */
#define HURRA_TYPE_TLM_AXIS           0x80
#define HURRA_TYPE_TLM_BUTTONS        0x81
#define HURRA_TYPE_TLM_MOUSE          0x82
#define HURRA_TYPE_TLM_KB             0x83
#define HURRA_TYPE_TLM_STATS          0x84
#define HURRA_TYPE_TLM_LOG            0x85
#define HURRA_TYPE_TLM_PHYS_AXIS      0x86  /* physical-only axis telemetry (firmware-pending) */
#define HURRA_TYPE_TLM_PHYS_BUTTONS   0x87  /* physical-only button telemetry (firmware-pending) */
#define HURRA_TYPE_TLM_PHYS_KB        0x88  /* physical-only keyboard telemetry (firmware-pending) */

/* Wire stats payload (TYPE 0x02 reply, TYPE 0x84 push).
 * 36 bytes — must match pack_stats() in firmware.  Little-endian fields.
 */
typedef struct {
    uint32_t uptime_ms;
    uint32_t rx_frames_ok;
    uint32_t head_crc_err;
    uint32_t payload_crc_err;
    uint32_t id_gap_total;
    uint32_t idle_resync;
    uint32_t rx_drv_overrun;
    uint32_t tx_ring_skip;
    uint32_t payload_invalid;
} hurra_stats_t;

#ifdef __cplusplus
}
#endif

#endif /* HURRA_TYPES_H */
