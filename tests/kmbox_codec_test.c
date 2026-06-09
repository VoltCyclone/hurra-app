/* KMBox codec unit tests.
 *   cc -std=c99 -Wall -Wextra -Isrc -o build/kmbox_codec_test \
 *      tests/kmbox_codec_test.c src/kmbox_codec.c
 */
#include "kmbox_codec.h"
#include <stdio.h>
#include <string.h>

static int g_fail = 0;
#define CHECK(cond) do { if (!(cond)) { \
    printf("FAIL %s:%d  %s\n", __FILE__, __LINE__, #cond); g_fail = 1; } } while (0)

/* Little-endian uint32 writer for building test packets. */
static void put_u32le(uint8_t *p, uint32_t v) {
    p[0]=(uint8_t)v; p[1]=(uint8_t)(v>>8); p[2]=(uint8_t)(v>>16); p[3]=(uint8_t)(v>>24);
}
static void put_i32le(uint8_t *p, int32_t v) { put_u32le(p, (uint32_t)v); }

static void test_short_buffer(void) {
    uint8_t buf[8] = {0};
    km_decoded_t d = km_decode(buf, sizeof buf);
    CHECK(!d.valid);
}

static void test_header_decode(void) {
    uint8_t buf[KM_HEAD_SIZE];
    put_u32le(&buf[0],  0x24BA5054u);   /* mac  */
    put_u32le(&buf[4],  0x11111111u);   /* rand */
    put_u32le(&buf[8],  0x00000007u);   /* indexpts */
    put_u32le(&buf[12], KM_CMD_CONNECT);
    km_decoded_t d = km_decode(buf, sizeof buf);
    CHECK(d.valid);
    CHECK(d.head.mac == 0x24BA5054u);
    CHECK(d.head.indexpts == 7);
    CHECK(d.head.cmd == KM_CMD_CONNECT);
}

static void test_ack_roundtrip(void) {
    km_head_t h = { 0xDEADBEEFu, 0x12345678u, 42, KM_CMD_MOUSE_MOVE };
    uint8_t out[KM_HEAD_SIZE];
    size_t n = km_build_ack(&h, out);
    CHECK(n == KM_HEAD_SIZE);
    km_decoded_t d = km_decode(out, n);
    CHECK(d.valid);
    CHECK(d.head.cmd == KM_CMD_MOUSE_MOVE);
    CHECK(d.head.indexpts == 42);
}

static void test_mouse_move_payload(void) {
    uint8_t buf[KM_HEAD_SIZE + KM_MOUSE_PAYLOAD_SIZE] = {0};
    put_u32le(&buf[12], KM_CMD_MOUSE_MOVE);
    put_i32le(&buf[KM_HEAD_SIZE + 0],  0);     /* button */
    put_i32le(&buf[KM_HEAD_SIZE + 4],  100);   /* x */
    put_i32le(&buf[KM_HEAD_SIZE + 8], -50);    /* y */
    put_i32le(&buf[KM_HEAD_SIZE + 12], 0);     /* wheel */
    km_decoded_t d = km_decode(buf, sizeof buf);
    CHECK(d.valid);
    CHECK(d.x == 100);
    CHECK(d.y == -50);
}

int main(void) {
    test_short_buffer();
    test_header_decode();
    test_ack_roundtrip();
    test_mouse_move_payload();
    if (g_fail) { printf("TESTS FAILED\n"); return 1; }
    printf("ALL TESTS PASSED\n");
    return 0;
}
