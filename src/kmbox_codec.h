/*
 * kmbox_codec.h — pure KMBox Net packet (de)serialization. No I/O, no hurra.
 */
#ifndef HURRA_KMBOX_CODEC_H
#define HURRA_KMBOX_CODEC_H

#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>
#include "kmbox_proto.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Decoded command. `valid` is false if the buffer was too short for a header. */
typedef struct {
    bool      valid;
    km_head_t head;
    /* Decoded mouse payload (present for mouse_* / automove / bezier cmds). */
    int32_t   button, x, y, wheel;
    /* Decoded keyboard payload (present for keyboard_all). */
    uint8_t   kb_ctrl;
    uint8_t   kb_keys[KM_KB_MAX_KEYS];
    int       kb_nkeys;          /* count of non-zero key slots */
    /* Bezier extras (present for bezier cmd). */
    uint16_t  dur_ms;
    int16_t   bz_x1, bz_y1, bz_x2, bz_y2;
} km_decoded_t;

/* Parse `len` bytes from `buf`. Always reads the 16-byte header if present;
 * decodes the payload relevant to head.cmd when enough bytes are available.
 * Returns the decoded struct; .valid=false when len < KM_HEAD_SIZE. */
km_decoded_t km_decode(const uint8_t *buf, size_t len);

/* Build the 16-byte ACK (echo of the request header) into out[KM_HEAD_SIZE].
 * Returns KM_HEAD_SIZE. */
size_t km_build_ack(const km_head_t *req, uint8_t out[KM_HEAD_SIZE]);

#ifdef __cplusplus
}
#endif

#endif /* HURRA_KMBOX_CODEC_H */
