#include "kmbox_codec.h"
#include <string.h>

static uint32_t get_u32le(const uint8_t *p) {
    return (uint32_t)p[0] | ((uint32_t)p[1]<<8) |
           ((uint32_t)p[2]<<16) | ((uint32_t)p[3]<<24);
}
static int32_t get_i32le(const uint8_t *p) {
    uint32_t u = get_u32le(p);
    int32_t v;
    memcpy(&v, &u, sizeof v);
    return v;
}
static void put_u32le(uint8_t *p, uint32_t v) {
    p[0]=(uint8_t)v; p[1]=(uint8_t)(v>>8); p[2]=(uint8_t)(v>>16); p[3]=(uint8_t)(v>>24);
}

km_decoded_t km_decode(const uint8_t *buf, size_t len) {
    km_decoded_t d;
    memset(&d, 0, sizeof d);
    if (!buf || len < KM_HEAD_SIZE) { d.valid = false; return d; }
    d.valid = true;
    d.head.mac      = get_u32le(&buf[0]);
    d.head.rand     = get_u32le(&buf[4]);
    d.head.indexpts = get_u32le(&buf[8]);
    d.head.cmd      = get_u32le(&buf[12]);

    const uint8_t *pl = buf + KM_HEAD_SIZE;
    size_t plen = len - KM_HEAD_SIZE;

    switch (d.head.cmd) {
        case KM_CMD_MOUSE_MOVE:
        case KM_CMD_MOUSE_LEFT:
        case KM_CMD_MOUSE_RIGHT:
        case KM_CMD_MOUSE_MIDDLE:
        case KM_CMD_MOUSE_WHEEL:
        case KM_CMD_MOUSE_AUTOMOVE:
        case KM_CMD_BEZIER:
            if (plen >= KM_MOUSE_CORE_SIZE) {   /* button,x,y,wheel are the first 4 int32 */
                d.button = get_i32le(&pl[0]);
                d.x      = get_i32le(&pl[4]);
                d.y      = get_i32le(&pl[8]);
                d.wheel  = get_i32le(&pl[12]);
            }
            break;
        case KM_CMD_KEYBOARD_ALL:
            if (plen >= KM_KB_PAYLOAD_SIZE) {
                d.kb_ctrl = pl[0];
                /* pl[1] is resvel (reserved); keys start at pl[2]. */
                for (int i = 0; i < KM_KB_MAX_KEYS; i++) {
                    uint8_t k = pl[2 + i];
                    d.kb_keys[i] = k;
                    if (k) d.kb_nkeys++;
                }
            }
            break;
        default:
            break;
    }
    return d;
}

size_t km_build_ack(const km_head_t *req, uint8_t out[KM_HEAD_SIZE]) {
    put_u32le(&out[0],  req->mac);
    put_u32le(&out[4],  req->rand);
    put_u32le(&out[8],  req->indexpts);
    put_u32le(&out[12], req->cmd);
    return KM_HEAD_SIZE;
}
