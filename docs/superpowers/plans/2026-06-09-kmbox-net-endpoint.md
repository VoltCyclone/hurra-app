# KMBox Net (UDP) Endpoint Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Add a KMBox Net UDP endpoint to the single `hurra-bridge` binary, selectable at startup alongside the existing VCOM/Ferrum endpoint, sharing one device link.

**Architecture:** Refactor the bridge into three layers — a per-protocol *frontend* (owns its transport + codec), a shared *translation core* (`input_core`, abstract input events → `hurra_*` calls), and `libhurra`. A startup TUI selector picks one frontend per run. The KMBox frontend emulates the device side of the KMBox Net UDP protocol; advanced commands needing capability the firmware lacks today (auto/bezier move, monitor, mask) ACK + log `pending firmware` until the co-designed Hurra frames land in the separate Hurra-v2 repo. No host-side emulation of device behaviour.

**Tech Stack:** C99, CMake, TinyFrame, POSIX/Winsock UDP sockets, existing `ferrum_parser` + `virtual_port` + `libhurra`. Tests are plain C executables run via CTest (new wiring).

**Scope:** Host-side only. The four new Hurra-protocol frames (`MOUSE_MOVE_DUR 0x1B`, `MOUSE_MOVE_BEZIER 0x1C`, `CB_PHYS 0x77` + `TLM_PHYS_* 0x86/0x87/0x88`, `PHYS_MASK 0x68`) and their firmware behaviour are a tracked Hurra-v2 dependency (separate plan); this plan only reserves the wire codes and wires the ACK+log-pending path.

**Reference:** Spec at `docs/superpowers/specs/2026-06-09-kmbox-net-endpoint-design.md`. KMBox Net protocol from <https://github.com/ZCban/kmboxNET>.

---

## File structure

| File | Responsibility |
|---|---|
| `src/kmbox_proto.h` | KMBox Net wire constants (cmd codes) + packed-free struct field offsets + `km_head_t`. Shared by codec + tests. |
| `src/kmbox_codec.{h,c}` | Pure (de)serialization: parse header+payload from a byte buffer into a decoded-command struct; build a 16-byte ACK. No I/O. |
| `src/input_core.{h,c}` | `input_sink_t` vtable + a concrete sink backed by one `hurra_client_t`. The `cb_*` translation lifted from `bridge.c`. |
| `src/frontend.h` | Frontend interface: `fe_poll`, `fe_close`, `fe_describe`, plus the `frontend_t` struct. |
| `src/frontend_vcom.{h,c}` | VCOM frontend: wraps `virtual_port` + `ferrum_parser`, drives `input_sink_t`. |
| `src/frontend_kmbox.{h,c}` | KMBox frontend: owns `udp_socket` + `kmbox_codec`, decodes → sink, echoes ACK, logs pending. |
| `src/udp_socket.h` + `src/udp_socket_unix.c` / `src/udp_socket_win32.c` | Non-blocking UDP abstraction (bind, recvfrom, sendto), per-OS like `virtual_port`. |
| `src/selector.{h,c}` | Startup endpoint TUI; returns the chosen mode or an error if no TTY. |
| `src/bridge.c` | Slimmed: args → ui_init → selector → build frontend + core → main loop (`fe_poll` + `hurra_poll`). |
| `include/hurra_types.h` | Reserve new TYPE codes (comment-only for firmware-pending ones). |
| `tests/kmbox_codec_test.c` | Codec unit tests. |
| `tests/input_core_test.c` | Translation tests with a mock sink. |
| `tests/CMakeLists` wiring | CTest registration for all three test execs (incl. existing ui_util). |

**Build/test convention (this repo):** tests are standalone C programs compiled with `cc -std=c99 -Wall -Wextra -Isrc -Iinclude`. We add CTest entries so `ctest` runs them; CI gains a test step.

---

## Task 1: Reserve new wire TYPE codes

**Files:**
- Modify: `include/hurra_types.h`

- [ ] **Step 1: Add the new TYPE constants**

In `include/hurra_types.h`, in the Mouse block (after `HURRA_TYPE_HUMAN 0x1A`, line 40) add:

```c
#define HURRA_TYPE_MOUSE_MOVE_DUR     0x1B  /* int16 dx,dy + uint16 dur_ms (firmware-pending) */
#define HURRA_TYPE_MOUSE_MOVE_BEZIER  0x1C  /* int16 dx,dy,uint16 dur_ms,int16 x1,y1,x2,y2 (firmware-pending) */
```

In the Locks block (after `HURRA_TYPE_CATCH_XY 0x67`, line 66) add:

```c
#define HURRA_TYPE_PHYS_MASK          0x68  /* uint8 domain,code,enable — enforce physical mask (firmware-pending) */
```

In the Stream/callback block (after `HURRA_TYPE_CB_KEYS 0x76`, line 75) add:

```c
#define HURRA_TYPE_CB_PHYS            0x77  /* enable physical-only telemetry (firmware-pending) */
```

In the telemetry block (after `HURRA_TYPE_TLM_LOG 0x85`, line 83) add:

```c
#define HURRA_TYPE_TLM_PHYS_AXIS      0x86  /* physical-only axis telemetry (firmware-pending) */
#define HURRA_TYPE_TLM_PHYS_BUTTONS   0x87  /* physical-only button telemetry (firmware-pending) */
#define HURRA_TYPE_TLM_PHYS_KB        0x88  /* physical-only keyboard telemetry (firmware-pending) */
```

- [ ] **Step 2: Verify it still compiles**

Run: `cmake -S . -B build && cmake --build build 2>&1 | tail -5`
Expected: build succeeds (header-only change, no usages yet).

- [ ] **Step 3: Commit**

```bash
git add include/hurra_types.h
git commit -m "feat: reserve KMBox-driven Hurra TYPE codes (firmware-pending)"
```

---

## Task 2: KMBox wire constants + header struct

**Files:**
- Create: `src/kmbox_proto.h`

- [ ] **Step 1: Write the constants header**

```c
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
#define KM_KB_PAYLOAD_SIZE    12
#define KM_KB_MAX_KEYS        10

#ifdef __cplusplus
}
#endif

#endif /* HURRA_KMBOX_PROTO_H */
```

- [ ] **Step 2: Verify it compiles standalone**

Run: `cc -std=c99 -Wall -Wextra -fsyntax-only -Isrc -x c src/kmbox_proto.h`
Expected: no output (clean).

- [ ] **Step 3: Commit**

```bash
git add src/kmbox_proto.h
git commit -m "feat: add KMBox Net wire constants and header layout"
```

---

## Task 3: KMBox codec — decode header

**Files:**
- Create: `src/kmbox_codec.h`
- Create: `src/kmbox_codec.c`
- Create: `tests/kmbox_codec_test.c`

- [ ] **Step 1: Write the codec header**

```c
/*
 * kmbox_codec.h — pure KMBox Net packet (de)serialization. No I/O, no hurra.
 */
#ifndef HURRA_KMBOX_CODEC_H
#define HURRA_KMBOX_CODEC_H

#include <staddef.h>
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
```

Note: fix the include typo when writing — it must be `#include <stddef.h>`.

- [ ] **Step 2: Write the failing test (header decode + ACK roundtrip)**

`tests/kmbox_codec_test.c`:

```c
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
```

- [ ] **Step 3: Run test, verify it fails to link**

Run: `cc -std=c99 -Wall -Wextra -Isrc -o build/kmbox_codec_test tests/kmbox_codec_test.c src/kmbox_codec.c 2>&1 | tail -3`
Expected: FAIL — `src/kmbox_codec.c` does not exist yet.

- [ ] **Step 4: Implement the codec**

`src/kmbox_codec.c`:

```c
#include "kmbox_codec.h"
#include <string.h>

static uint32_t get_u32le(const uint8_t *p) {
    return (uint32_t)p[0] | ((uint32_t)p[1]<<8) |
           ((uint32_t)p[2]<<16) | ((uint32_t)p[3]<<24);
}
static int32_t get_i32le(const uint8_t *p) { return (int32_t)get_u32le(p); }
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
            if (plen >= 16) {   /* button,x,y,wheel are the first 4 int32 */
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
```

- [ ] **Step 5: Run test, verify pass**

Run: `cc -std=c99 -Wall -Wextra -Isrc -o build/kmbox_codec_test tests/kmbox_codec_test.c src/kmbox_codec.c && ./build/kmbox_codec_test`
Expected: `ALL TESTS PASSED`

- [ ] **Step 6: Commit**

```bash
git add src/kmbox_codec.h src/kmbox_codec.c tests/kmbox_codec_test.c
git commit -m "feat: KMBox Net packet codec with unit tests"
```

---

## Task 4: input_sink interface + mock-backed translation tests

**Files:**
- Create: `src/input_core.h`
- Create: `src/input_core.c`
- Create: `tests/input_core_test.c`

This task defines the abstract `input_sink_t` and a test that drives it through a **mock** (capturing) implementation. The real hurra-backed sink comes in Task 5.

- [ ] **Step 1: Write `src/input_core.h`**

```c
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
} input_sink_t;

/* Construct a sink that forwards to a hurra_client_t. `hc` is borrowed.
 * Returns 0 on success and fills *out. */
struct hurra_client;  /* fwd decl; real type from hurra.h */
int  input_core_init(input_sink_t *out, struct hurra_client *hc);

#ifdef __cplusplus
}
#endif

#endif /* HURRA_INPUT_CORE_H */
```

- [ ] **Step 2: Write the failing test with a mock sink**

`tests/input_core_test.c`:

```c
/* input_core translation tests via a mock sink.
 *   cc -std=c99 -Wall -Wextra -Isrc -o build/input_core_test tests/input_core_test.c
 * (No hurra link needed: this tests the sink CONTRACT using a mock.)
 */
#include "input_core.h"
#include <stdio.h>
#include <string.h>

static int g_fail = 0;
#define CHECK(c) do { if(!(c)){printf("FAIL %s:%d %s\n",__FILE__,__LINE__,#c);g_fail=1;} } while(0)

/* Mock sink: record the last call of each kind. */
typedef struct {
    int  moves; int32_t last_dx, last_dy;
    int  buttons; int last_btn, last_down;
    int  wheels;  int32_t last_ticks;
    int  kb_reports; uint8_t last_mod; int last_nkeys;
    int  reboots;
} mock_t;

static void m_move(input_sink_t *s, int32_t dx, int32_t dy){ mock_t*m=s->ctx; m->moves++; m->last_dx=dx; m->last_dy=dy; }
static void m_btn(input_sink_t *s, int b, int d){ mock_t*m=s->ctx; m->buttons++; m->last_btn=b; m->last_down=d; }
static void m_wheel(input_sink_t *s, int32_t t){ mock_t*m=s->ctx; m->wheels++; m->last_ticks=t; }
static void m_all(input_sink_t *s, uint8_t b, int32_t dx, int32_t dy, int32_t w){ (void)b;(void)dx;(void)dy;(void)w; mock_t*m=s->ctx; m->moves++; }
static void m_kb(input_sink_t *s, uint8_t mod, const uint8_t*k, int n){ (void)k; mock_t*m=s->ctx; m->kb_reports++; m->last_mod=mod; m->last_nkeys=n; }
static void m_reboot(input_sink_t *s){ mock_t*m=s->ctx; m->reboots++; }

static input_sink_t make_mock(mock_t *m) {
    input_sink_t s; memset(&s,0,sizeof s);
    s.ctx=m; s.move=m_move; s.button=m_btn; s.wheel=m_wheel;
    s.mouse_all=m_all; s.kb_report=m_kb; s.reboot=m_reboot;
    return s;
}

/* The decode→sink mapping under test lives in the frontends, but the SINK
 * contract is what both share. Here we assert the mock honors the vtable so
 * frontend tests can rely on it. */
static void test_mock_dispatch(void) {
    mock_t m; memset(&m,0,sizeof m);
    input_sink_t s = make_mock(&m);
    s.move(&s, 100, -50);
    s.button(&s, 1, 1);
    s.wheel(&s, 3);
    uint8_t keys[2] = {0x04, 0x05};
    s.kb_report(&s, 0x02, keys, 2);
    s.reboot(&s);
    CHECK(m.moves == 1 && m.last_dx == 100 && m.last_dy == -50);
    CHECK(m.buttons == 1 && m.last_btn == 1 && m.last_down == 1);
    CHECK(m.wheels == 1 && m.last_ticks == 3);
    CHECK(m.kb_reports == 1 && m.last_mod == 0x02 && m.last_nkeys == 2);
    CHECK(m.reboots == 1);
}

int main(void) {
    test_mock_dispatch();
    if (g_fail) { printf("TESTS FAILED\n"); return 1; }
    printf("ALL TESTS PASSED\n");
    return 0;
}
```

- [ ] **Step 3: Run test, verify it fails to compile**

Run: `cc -std=c99 -Wall -Wextra -Isrc -o build/input_core_test tests/input_core_test.c 2>&1 | tail -3`
Expected: FAIL — `input_core.h` not found / `input_sink_t` undefined until Step 1 file saved; if header exists, it compiles. (If Step 1 already saved, this passes — that's fine; the contract test is the deliverable.)

- [ ] **Step 4: Run test, verify pass**

Run: `cc -std=c99 -Wall -Wextra -Isrc -o build/input_core_test tests/input_core_test.c && ./build/input_core_test`
Expected: `ALL TESTS PASSED`

- [ ] **Step 5: Commit**

```bash
git add src/input_core.h tests/input_core_test.c
git commit -m "feat: define input_sink interface with contract test"
```

---

## Task 5: hurra-backed sink (lift cb_* translation from bridge.c)

**Files:**
- Create: `src/input_core.c`
- Reference (do not yet delete from): `src/bridge.c:283-321` (`cb_move`, `cb_button_set`, `cb_wheel`)

- [ ] **Step 1: Implement `input_core.c` forwarding to hurra**

```c
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
    /* Modifier byte → individual modifier HID downs (0xE0..0xE7). */
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
    out->ctx       = hc;
    out->move      = core_move;
    out->button    = core_button;
    out->wheel     = core_wheel;
    out->mouse_all = core_mouse_all;
    out->kb_report = core_kb_report;
    out->reboot    = core_reboot;
    return 0;
}
```

Note: `s->ctx` stores the `hurra_client_t*` directly (set in `input_core_init`); the mock test used `ctx` for its own struct — both are fine because each sink owns its ctx.

- [ ] **Step 2: Add to CMake bridge sources**

In `CMakeLists.txt`, in the `BRIDGE_SOURCES` list (line 47-50), add `src/input_core.c`:

```cmake
set(BRIDGE_SOURCES
    src/bridge.c
    src/ferrum_parser.c
    src/input_core.c
)
```

- [ ] **Step 3: Verify build**

Run: `cmake -S . -B build && cmake --build build 2>&1 | tail -5`
Expected: build succeeds (input_core.c compiles and links against hurra; not yet called from bridge.c).

- [ ] **Step 4: Commit**

```bash
git add src/input_core.c CMakeLists.txt
git commit -m "feat: hurra-backed input sink (move/button/wheel/mouse_all/kb/reboot)"
```

---

## Task 6: UDP socket abstraction (Unix)

**Files:**
- Create: `src/udp_socket.h`
- Create: `src/udp_socket_unix.c`

(Windows variant is Task 11; Unix-first keeps the loop testable on the dev box.)

- [ ] **Step 1: Write `src/udp_socket.h`**

```c
/*
 * udp_socket.h — minimal non-blocking UDP socket the KMBox frontend owns.
 * Server-side: bind a local port, recvfrom clients, sendto the last client.
 */
#ifndef HURRA_UDP_SOCKET_H
#define HURRA_UDP_SOCKET_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct udp_socket udp_socket_t;

/* Opaque client address (sockaddr_storage under the hood). */
typedef struct { unsigned char bytes[128]; unsigned int len; } udp_addr_t;

/* Bind UDP on bind_addr:port (e.g. "0.0.0.0", 12345). NULL addr → 0.0.0.0.
 * Returns NULL on failure (errno set). */
udp_socket_t *udp_open(const char *bind_addr, uint16_t port);
void          udp_close(udp_socket_t *s);

/* Non-blocking recv. Returns bytes read (>0), 0 if no datagram waiting,
 * -1 on hard error. Fills *from with the sender address. */
int udp_recv(udp_socket_t *s, uint8_t *buf, size_t n, udp_addr_t *from);

/* Send to a specific address. Returns bytes sent or -1. */
int udp_send(udp_socket_t *s, const uint8_t *buf, size_t n, const udp_addr_t *to);

#ifdef __cplusplus
}
#endif

#endif /* HURRA_UDP_SOCKET_H */
```

- [ ] **Step 2: Implement `src/udp_socket_unix.c`**

```c
#include "udp_socket.h"
#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

struct udp_socket { int fd; };

static int set_nonblock(int fd) {
    int fl = fcntl(fd, F_GETFL, 0);
    if (fl < 0) return -1;
    return fcntl(fd, F_SETFL, fl | O_NONBLOCK);
}

udp_socket_t *udp_open(const char *bind_addr, uint16_t port) {
    int fd = socket(AF_INET, SOCK_DGRAM, 0);
    if (fd < 0) return NULL;

    struct sockaddr_in sa;
    memset(&sa, 0, sizeof sa);
    sa.sin_family = AF_INET;
    sa.sin_port   = htons(port);
    if (!bind_addr || !*bind_addr) sa.sin_addr.s_addr = htonl(INADDR_ANY);
    else if (inet_pton(AF_INET, bind_addr, &sa.sin_addr) != 1) {
        close(fd); errno = EINVAL; return NULL;
    }
    if (bind(fd, (struct sockaddr *)&sa, sizeof sa) < 0) {
        int e = errno; close(fd); errno = e; return NULL;
    }
    if (set_nonblock(fd) < 0) { int e = errno; close(fd); errno = e; return NULL; }

    udp_socket_t *s = calloc(1, sizeof *s);
    if (!s) { close(fd); errno = ENOMEM; return NULL; }
    s->fd = fd;
    return s;
}

void udp_close(udp_socket_t *s) {
    if (!s) return;
    if (s->fd >= 0) close(s->fd);
    free(s);
}

int udp_recv(udp_socket_t *s, uint8_t *buf, size_t n, udp_addr_t *from) {
    if (!s) return -1;
    struct sockaddr_storage ss;
    socklen_t sl = sizeof ss;
    ssize_t r = recvfrom(s->fd, buf, n, 0, (struct sockaddr *)&ss, &sl);
    if (r < 0) {
        if (errno == EAGAIN || errno == EWOULDBLOCK) return 0;
        return -1;
    }
    if (from) {
        if (sl > sizeof from->bytes) sl = sizeof from->bytes;
        memcpy(from->bytes, &ss, sl);
        from->len = (unsigned int)sl;
    }
    return (int)r;
}

int udp_send(udp_socket_t *s, const uint8_t *buf, size_t n, const udp_addr_t *to) {
    if (!s || !to || to->len == 0) return -1;
    ssize_t w = sendto(s->fd, buf, n, 0,
                       (const struct sockaddr *)to->bytes, (socklen_t)to->len);
    return (w < 0) ? -1 : (int)w;
}
```

- [ ] **Step 3: Smoke-test the socket with a loopback program**

Run:
```bash
cat > /tmp/udp_smoke.c <<'EOF'
#include "udp_socket.h"
#include <stdio.h>
#include <string.h>
int main(void){
    udp_socket_t *srv = udp_open("127.0.0.1", 0);  /* ephemeral not supported here; use fixed */
    (void)srv;
    udp_socket_t *s = udp_open("127.0.0.1", 39111);
    if(!s){ printf("open FAIL\n"); return 1; }
    udp_close(s);
    printf("OPEN/CLOSE OK\n");
    return 0;
}
EOF
cc -std=c99 -Wall -Wextra -Isrc -o build/udp_smoke /tmp/udp_smoke.c src/udp_socket_unix.c && ./build/udp_smoke
```
Expected: `OPEN/CLOSE OK` (proves bind + close work; full loopback recv/send is exercised in Task 10).

- [ ] **Step 4: Commit**

```bash
git add src/udp_socket.h src/udp_socket_unix.c
git commit -m "feat: non-blocking UDP socket abstraction (Unix)"
```

---

## Task 7: Frontend interface + VCOM frontend (refactor existing path)

**Files:**
- Create: `src/frontend.h`
- Create: `src/frontend_vcom.h`, `src/frontend_vcom.c`
- Modify: `src/bridge.c` (extract ferrum callbacks to call the sink; this task only ADDS the frontend, wiring into main is Task 9)

- [ ] **Step 1: Write `src/frontend.h`**

```c
/*
 * frontend.h — common interface for an input endpoint (transport + protocol).
 * The bridge constructs exactly one frontend per run and pumps it each tick.
 */
#ifndef HURRA_FRONTEND_H
#define HURRA_FRONTEND_H

#include <stdbool.h>
#include "input_core.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct frontend frontend_t;

struct frontend {
    void *impl;
    /* One non-blocking pump: drain transport, decode, drive the sink.
     * Returns >0 if work was done, 0 if idle, -1 on hard error. */
    int  (*poll) (frontend_t *fe);
    void (*close)(frontend_t *fe);
    /* Borrowed human-readable description for the banner (e.g. the PTY path
     * or the UDP listen addr). */
    const char *(*describe)(frontend_t *fe);
};

#ifdef __cplusplus
}
#endif

#endif /* HURRA_FRONTEND_H */
```

- [ ] **Step 2: Write `src/frontend_vcom.h`**

```c
#ifndef HURRA_FRONTEND_VCOM_H
#define HURRA_FRONTEND_VCOM_H
#include "frontend.h"
#ifdef __cplusplus
extern "C" {
#endif
/* Open the VCOM frontend. `link_path` (Unix) or `virtual_port` (Win) per OS,
 * passed through to vp_open. The sink is borrowed. Returns 0 on success. */
int frontend_vcom_open(frontend_t *out, input_sink_t *sink,
                       const char *vp_arg, const char *link_path,
                       int request_timeout_ms);
/* The PTY slave path / COM name, for the banner. */
const char *frontend_vcom_slave_path(frontend_t *fe);
#ifdef __cplusplus
}
#endif
#endif
```

- [ ] **Step 3: Implement `src/frontend_vcom.c`**

This wraps `virtual_port` + `ferrum_parser`, mapping ferrum callbacks onto the sink. The ferrum callback table requires the full set (get-style replies still need the writer); for get-style and telemetry, keep the existing emit helpers by having the vcom frontend own its own writer to `vp_write`. Movement/button/wheel/reboot route through the sink.

```c
#include "frontend_vcom.h"
#include "ferrum_parser.h"
#include "virtual_port.h"
#include "hurra.h"
#include <stdlib.h>
#include <string.h>

typedef struct {
    vp_port_t       *vp;
    ferrum_parser_t *parser;
    input_sink_t    *sink;
    hurra_client_t  *hc;        /* for get-style replies + telemetry */
    int              timeout_ms;
} vcom_t;

/* ferrum write adapter → vp_write_all */
static void vcom_write(const uint8_t *buf, size_t n, void *user) {
    vcom_t *v = (vcom_t *)user;
    size_t off = 0;
    while (off < n) {
        int w = vp_write(v->vp, buf + off, n - off);
        if (w <= 0) return;
        off += (size_t)w;
    }
}

/* --- ferrum callbacks that route to the sink --- */
static void fv_move(int32_t x, int32_t y, void *u){ vcom_t*v=u; v->sink->move(v->sink, x, y); }
static void fv_wheel(int32_t n, void *u){ vcom_t*v=u; v->sink->wheel(v->sink, n); }

/* map ferrum button mask bit → 0..4 */
static int btn_idx(uint8_t mask){ switch(mask){case 0x01:return 0;case 0x02:return 1;
    case 0x04:return 2;case 0x08:return 3;case 0x10:return 4;default:return -1;} }
static void fv_button_set(uint8_t mask, uint8_t st, void *u){
    vcom_t*v=u; int i=btn_idx(mask); if(i>=0) v->sink->button(v->sink, i, st); }
```

The remaining ferrum callbacks (get-style: `on_version`, `on_button_get`, `on_lock_*`, `on_catch_xy`, `on_kb_isdown`, `on_cb_*_get`, etc.) keep their existing implementations from `bridge.c` but call `hurra_*` directly via `v->hc` and emit via `vcom_write`. **Copy each existing `cb_*` body from `src/bridge.c:264-427` into static `fv_*` functions here, replacing `b->hc` with `v->hc`, `b->vp` writes with `vcom_write`, and `writer_for_emit, b` with `vcom_write, v`.** Telemetry callbacks (`on_tlm_*`) likewise move here, registered via `hurra_on_telemetry` in `frontend_vcom_open`.

`frontend_vcom_open` builds the `ferrum_callbacks_t` wiring `on_move=fv_move`, `on_wheel=fv_wheel`, `on_button_set=fv_button_set`, and the copied `fv_*` for the rest; opens `vp_open(vp_arg, link_path)`; registers telemetry; stores everything in a heap `vcom_t`; sets `out->poll/close/describe`.

`vcom_poll` does today's read loop body (`vp_read` → feed bytes to parser → `ferrum_parser_tick`), plus the `__diag__` side-channel handling, returning bytes read.

> **Note for implementer:** this is a mechanical extraction. Keep behaviour byte-identical to current `bridge.c`. The full current bodies are in `src/bridge.c`; do not paraphrase them — move them.

- [ ] **Step 4: Add to CMake and build**

Add `src/frontend_vcom.c` to `BRIDGE_SOURCES`. Run:
`cmake --build build 2>&1 | tail -5`
Expected: compiles (not yet used by main).

- [ ] **Step 5: Commit**

```bash
git add src/frontend.h src/frontend_vcom.h src/frontend_vcom.c CMakeLists.txt
git commit -m "feat: frontend interface + VCOM frontend (extracted from bridge.c)"
```

---

## Task 8: KMBox frontend

**Files:**
- Create: `src/frontend_kmbox.h`, `src/frontend_kmbox.c`

- [ ] **Step 1: Write `src/frontend_kmbox.h`**

```c
#ifndef HURRA_FRONTEND_KMBOX_H
#define HURRA_FRONTEND_KMBOX_H
#include "frontend.h"
#include <stdint.h>
#ifdef __cplusplus
extern "C" {
#endif
/* Open the KMBox Net frontend: bind UDP on bind_addr:port, accept clients
 * presenting `mac` in connect. The sink is borrowed. Returns 0 on success;
 * -1 on bind failure (errno set). */
int frontend_kmbox_open(frontend_t *out, input_sink_t *sink,
                        const char *bind_addr, uint16_t port, uint32_t mac);
#ifdef __cplusplus
}
#endif
#endif
```

- [ ] **Step 2: Implement `src/frontend_kmbox.c`**

```c
#include "frontend_kmbox.h"
#include "kmbox_codec.h"
#include "udp_socket.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef struct {
    udp_socket_t *sock;
    input_sink_t *sink;
    uint32_t      mac;
    udp_addr_t    client;     /* last connected client (for monitor relay) */
    bool          have_client;
    char          desc[64];
    /* one-shot "pending firmware" log guards */
    bool          warned_automove, warned_bezier, warned_monitor, warned_mask;
    uint64_t      rx_bad;
} kmbox_t;

static void warn_once(bool *flag, const char *cmd) {
    if (!*flag) { fprintf(stderr, "kmbox: pending firmware: %s\n", cmd); *flag = true; }
}

static int km_poll(frontend_t *fe) {
    kmbox_t *k = (kmbox_t *)fe->impl;
    uint8_t buf[256];
    udp_addr_t from;
    int n = udp_recv(k->sock, buf, sizeof buf, &from);
    if (n <= 0) return n;   /* 0 idle, -1 error */

    km_decoded_t d = km_decode(buf, (size_t)n);
    if (!d.valid) { k->rx_bad++; return 1; }

    bool ack = true;
    switch (d.head.cmd) {
        case KM_CMD_CONNECT:
            if (k->mac != 0 && d.head.mac != k->mac) {
                /* wrong MAC: drop, do not ACK */
                fprintf(stderr, "kmbox: connect rejected (mac mismatch)\n");
                return 1;
            }
            k->client = from; k->have_client = true;
            break;
        case KM_CMD_MOUSE_MOVE:
            k->sink->move(k->sink, d.x, d.y); break;
        case KM_CMD_MOUSE_LEFT:
            k->sink->button(k->sink, 0, d.button & 0x01); break;
        case KM_CMD_MOUSE_RIGHT:
            k->sink->button(k->sink, 1, d.button & 0x02); break;
        case KM_CMD_MOUSE_MIDDLE:
            k->sink->button(k->sink, 2, d.button & 0x04); break;
        case KM_CMD_MOUSE_WHEEL:
            k->sink->wheel(k->sink, d.wheel); break;
        case KM_CMD_KEYBOARD_ALL:
            k->sink->kb_report(k->sink, d.kb_ctrl, d.kb_keys, d.kb_nkeys); break;
        case KM_CMD_REBOOT:
            k->sink->reboot(k->sink); break;
        /* Firmware-pending: ACK so clients don't error; log once. */
        case KM_CMD_MOUSE_AUTOMOVE: warn_once(&k->warned_automove, "automove"); break;
        case KM_CMD_BEZIER:         warn_once(&k->warned_bezier,   "bezier");   break;
        case KM_CMD_MONITOR:        warn_once(&k->warned_monitor,  "monitor");  break;
        case KM_CMD_MASK_MOUSE:
        case KM_CMD_UNMASK_ALL:     warn_once(&k->warned_mask,     "mask");     break;
        default:
            ack = false; k->rx_bad++; break;
    }

    if (ack) {
        uint8_t out[KM_HEAD_SIZE];
        size_t m = km_build_ack(&d.head, out);
        (void)udp_send(k->sock, out, m, &from);
    }
    return 1;
}

static const char *km_describe(frontend_t *fe) {
    return ((kmbox_t *)fe->impl)->desc;
}

static void km_close(frontend_t *fe) {
    kmbox_t *k = (kmbox_t *)fe->impl;
    if (!k) return;
    udp_close(k->sock);
    free(k);
    fe->impl = NULL;
}

int frontend_kmbox_open(frontend_t *out, input_sink_t *sink,
                        const char *bind_addr, uint16_t port, uint32_t mac) {
    kmbox_t *k = calloc(1, sizeof *k);
    if (!k) return -1;
    k->sock = udp_open(bind_addr, port);
    if (!k->sock) { free(k); return -1; }
    k->sink = sink;
    k->mac  = mac;
    snprintf(k->desc, sizeof k->desc, "UDP %s:%u",
             (bind_addr && *bind_addr) ? bind_addr : "0.0.0.0", (unsigned)port);
    out->impl     = k;
    out->poll     = km_poll;
    out->close    = km_close;
    out->describe = km_describe;
    return 0;
}
```

- [ ] **Step 3: Add to CMake (Unix list) and build**

Add `src/frontend_kmbox.c` and `src/udp_socket_unix.c` to the non-Windows `BRIDGE_SOURCES` branch. Run:
`cmake --build build 2>&1 | tail -5`
Expected: compiles.

- [ ] **Step 4: Commit**

```bash
git add src/frontend_kmbox.h src/frontend_kmbox.c CMakeLists.txt
git commit -m "feat: KMBox Net frontend (decode→sink, ACK, pending-firmware log)"
```

---

## Task 9: Startup selector + bridge main rewiring

**Files:**
- Create: `src/selector.h`, `src/selector.c`
- Modify: `src/bridge.c`

- [ ] **Step 1: Write `src/selector.h`**

```c
#ifndef HURRA_SELECTOR_H
#define HURRA_SELECTOR_H
#ifdef __cplusplus
extern "C" {
#endif
typedef enum { ENDPOINT_VCOM = 1, ENDPOINT_KMBOX = 2 } endpoint_t;
/* Render the endpoint menu to stderr and read a choice from stdin.
 * Returns the chosen endpoint, or -1 if stdin is not a TTY (caller errors). */
int selector_choose(void);   /* returns endpoint_t or -1 */
#ifdef __cplusplus
}
#endif
#endif
```

- [ ] **Step 2: Implement `src/selector.c`**

```c
#include "selector.h"
#include <stdio.h>
#ifdef _WIN32
#  include <io.h>
#  define ISATTY(fd) _isatty(fd)
#  define FILENO _fileno
#else
#  include <unistd.h>
#  define ISATTY(fd) isatty(fd)
#  define FILENO fileno
#endif

int selector_choose(void) {
    if (!ISATTY(FILENO(stdin))) return -1;
    fprintf(stderr,
        "\nhurra-bridge\n\n"
        "  Select an endpoint:\n"
        "    1) Virtual COM port (Ferrum-compatible)   [default]\n"
        "    2) KMBox Net (UDP)\n\n  > ");
    fflush(stderr);
    int c = fgetc(stdin);
    /* consume rest of line */
    int x = c; while (x != '\n' && x != EOF) x = fgetc(stdin);
    if (c == '2') return ENDPOINT_KMBOX;
    return ENDPOINT_VCOM;   /* '1', Enter, anything else → default */
}
```

- [ ] **Step 3: Rewire `bridge.c` main**

In `src/bridge.c`:
1. Add includes: `#include "frontend.h"`, `#include "frontend_vcom.h"`, `#include "frontend_kmbox.h"`, `#include "input_core.h"`, `#include "selector.h"`.
2. Near the top of `bridge.c` add `#define KM_DEFAULT_PORT 16896` (the bridge's own default; documented in usage + README). Add KMBox args to `args_t` and `parse_args`: `const char *km_bind; uint16_t km_port; uint32_t km_mac;` with defaults `km_bind="0.0.0.0"`, `km_port=KM_DEFAULT_PORT`, `km_mac=0` (0 = accept any). Parse `--km-port` (`(uint16_t)strtoul(arg,NULL,10)`), `--km-bind`, `--km-mac` (`(uint32_t)strtoul(arg,NULL,16)`).
3. After `ui_init` and opening `br.hc` (keep serial-open logic), call:
   ```c
   int ep = selector_choose();
   if (ep < 0) {
       hurra_close(br.hc);
       return bridge_fail(2, "No terminal to choose an endpoint",
           "  hurra-bridge now starts with an interactive endpoint menu.\n"
           "  -> Run it from an interactive terminal (TTY required).");
   }
   ```
4. Build the sink + frontend:
   ```c
   input_sink_t sink;
   input_core_init(&sink, br.hc);
   frontend_t fe; memset(&fe, 0, sizeof fe);
   if (ep == ENDPOINT_KMBOX) {
       if (frontend_kmbox_open(&fe, &sink, args.km_bind, args.km_port, args.km_mac) != 0) {
           hurra_close(br.hc);
           return bridge_fail(1, "Can't bind KMBox Net UDP port",
               "  The UDP port is in use or not permitted. Try --km-port N.");
       }
   } else {
       /* VCOM: keep existing link/virtual-port resolution, pass to frontend_vcom_open */
       const char *link = args.link_path ? args.link_path : default_link_path();
       if (frontend_vcom_open(&fe, &sink,
               args.virtual_port, link, args.timeout_ms) != 0) {
           hurra_close(br.hc);
           return bridge_fail(1, "Can't create the virtual serial port (PTY)", "");
       }
   }
   ```
5. Replace the old `vp_read`/parser loop body in the main `while (!g_stop)` with:
   ```c
   int did = fe.poll(&fe);
   if (did < 0) { blog("frontend poll error; exiting"); break; }
   int drained = hurra_poll(br.hc);
   ...
   if (did == 0 && drained == 0) sleep_us(500);
   ```
   Keep the status-line / heartbeat / link-health code unchanged. Add the mode tag to the status line by passing `fe.describe(&fe)` into the banner.
6. Replace banner `Virtual port` line to use `fe.describe(&fe)`; for VCOM keep existing detail lines.
7. At shutdown call `fe.close(&fe)` instead of `vp_close`.

> **Note:** the bulky `cb_*`, `on_tlm_*`, `writer_for_emit`, and the `__diag__`
> handling move into `frontend_vcom.c` (Task 7). After this task, `bridge.c` no
> longer references `ferrum_parser`/`vp_*` directly except through the frontend.

- [ ] **Step 4: Build and run the binary interactively**

Run: `cmake --build build 2>&1 | tail -5 && echo "1" | ./build/hurra-bridge --help 2>&1 | head -5`
Expected: build succeeds; `--help` prints usage (selector not reached for --help).

- [ ] **Step 5: Verify non-TTY errors cleanly**

Run: `printf '' | ./build/hurra-bridge --device /dev/null 2>&1 | head -3 ; echo "exit=$?"`
Expected: prints the "No terminal to choose an endpoint" message (or serial-open failure first if `/dev/null` rejected). The point: no hang, clean exit.

- [ ] **Step 6: Commit**

```bash
git add src/selector.h src/selector.c src/bridge.c
git commit -m "feat: startup endpoint selector + frontend-based main loop"
```

---

## Task 10: UDP loopback integration test

**Files:**
- Create: `tests/kmbox_loopback_test.c`

- [ ] **Step 1: Write the integration test (mock sink + real socket)**

```c
/* KMBox loopback: bind frontend, send connect+mouse_move from a client socket,
 * assert ACK returns and the mock sink saw the move.
 *   cc -std=c99 -Wall -Wextra -Isrc -o build/kmbox_loopback_test \
 *      tests/kmbox_loopback_test.c src/frontend_kmbox.c src/kmbox_codec.c src/udp_socket_unix.c
 */
#include "frontend_kmbox.h"
#include "kmbox_codec.h"
#include <arpa/inet.h>
#include <netinet/in.h>
#include <stdio.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

static int g_fail = 0;
#define CHECK(c) do{ if(!(c)){printf("FAIL %s:%d %s\n",__FILE__,__LINE__,#c);g_fail=1;} }while(0)

static int g_moves=0, g_last_x=0, g_last_y=0;
static void s_move(input_sink_t*s,int32_t dx,int32_t dy){(void)s;g_moves++;g_last_x=dx;g_last_y=dy;}
static void s_noop_btn(input_sink_t*s,int b,int d){(void)s;(void)b;(void)d;}
static void s_noop_wheel(input_sink_t*s,int32_t t){(void)s;(void)t;}
static void s_noop_all(input_sink_t*s,uint8_t b,int32_t x,int32_t y,int32_t w){(void)s;(void)b;(void)x;(void)y;(void)w;}
static void s_noop_kb(input_sink_t*s,uint8_t m,const uint8_t*k,int n){(void)s;(void)m;(void)k;(void)n;}
static void s_noop_reboot(input_sink_t*s){(void)s;}

static void put_u32le(uint8_t*p,uint32_t v){p[0]=v;p[1]=v>>8;p[2]=v>>16;p[3]=v>>24;}
static void put_i32le(uint8_t*p,int32_t v){put_u32le(p,(uint32_t)v);}

int main(void){
    const uint16_t PORT = 39222;
    input_sink_t sink; memset(&sink,0,sizeof sink);
    sink.move=s_move; sink.button=s_noop_btn; sink.wheel=s_noop_wheel;
    sink.mouse_all=s_noop_all; sink.kb_report=s_noop_kb; sink.reboot=s_noop_reboot;

    frontend_t fe; memset(&fe,0,sizeof fe);
    CHECK(frontend_kmbox_open(&fe,&sink,"127.0.0.1",PORT,0)==0);

    int cli = socket(AF_INET,SOCK_DGRAM,0);
    struct sockaddr_in srv; memset(&srv,0,sizeof srv);
    srv.sin_family=AF_INET; srv.sin_port=htons(PORT);
    inet_pton(AF_INET,"127.0.0.1",&srv.sin_addr);

    /* connect */
    uint8_t pkt[KM_HEAD_SIZE+KM_MOUSE_PAYLOAD_SIZE]; memset(pkt,0,sizeof pkt);
    put_u32le(&pkt[12],KM_CMD_CONNECT);
    sendto(cli,pkt,KM_HEAD_SIZE,0,(struct sockaddr*)&srv,sizeof srv);
    CHECK(fe.poll(&fe) > 0);
    uint8_t ack[64]; ssize_t r = recv(cli,ack,sizeof ack,0);
    CHECK(r==KM_HEAD_SIZE);

    /* mouse_move 100,-50 */
    memset(pkt,0,sizeof pkt);
    put_u32le(&pkt[8],1); put_u32le(&pkt[12],KM_CMD_MOUSE_MOVE);
    put_i32le(&pkt[KM_HEAD_SIZE+4],100); put_i32le(&pkt[KM_HEAD_SIZE+8],-50);
    sendto(cli,pkt,sizeof pkt,0,(struct sockaddr*)&srv,sizeof srv);
    CHECK(fe.poll(&fe) > 0);
    r = recv(cli,ack,sizeof ack,0);
    CHECK(r==KM_HEAD_SIZE);
    CHECK(g_moves==1 && g_last_x==100 && g_last_y==-50);

    close(cli);
    fe.close(&fe);
    if(g_fail){printf("TESTS FAILED\n");return 1;}
    printf("ALL TESTS PASSED\n");
    return 0;
}
```

- [ ] **Step 2: Build and run**

Run:
```bash
cc -std=c99 -Wall -Wextra -Isrc -o build/kmbox_loopback_test \
   tests/kmbox_loopback_test.c src/frontend_kmbox.c src/kmbox_codec.c src/udp_socket_unix.c && \
./build/kmbox_loopback_test
```
Expected: `ALL TESTS PASSED`

- [ ] **Step 3: Commit**

```bash
git add tests/kmbox_loopback_test.c
git commit -m "test: KMBox UDP loopback integration (connect + mouse_move)"
```

---

## Task 11: Windows UDP socket + CMake/CI wiring

**Files:**
- Create: `src/udp_socket_win32.c`
- Modify: `CMakeLists.txt`, `.github/workflows/ci.yml`

- [ ] **Step 1: Implement `src/udp_socket_win32.c`**

```c
#include "udp_socket.h"
#define WIN32_LEAN_AND_MEAN
#include <winsock2.h>
#include <ws2tcpip.h>
#include <stdlib.h>
#include <string.h>

struct udp_socket { SOCKET fd; };
static int g_wsa = 0;

udp_socket_t *udp_open(const char *bind_addr, uint16_t port) {
    if (!g_wsa) { WSADATA w; if (WSAStartup(MAKEWORD(2,2), &w) != 0) return NULL; g_wsa = 1; }
    SOCKET fd = socket(AF_INET, SOCK_DGRAM, 0);
    if (fd == INVALID_SOCKET) return NULL;
    struct sockaddr_in sa; memset(&sa,0,sizeof sa);
    sa.sin_family = AF_INET; sa.sin_port = htons(port);
    if (!bind_addr || !*bind_addr) sa.sin_addr.s_addr = htonl(INADDR_ANY);
    else if (inet_pton(AF_INET, bind_addr, &sa.sin_addr) != 1) { closesocket(fd); return NULL; }
    if (bind(fd, (struct sockaddr*)&sa, sizeof sa) == SOCKET_ERROR) { closesocket(fd); return NULL; }
    u_long nb = 1; ioctlsocket(fd, FIONBIO, &nb);
    udp_socket_t *s = calloc(1, sizeof *s); if (!s) { closesocket(fd); return NULL; }
    s->fd = fd; return s;
}
void udp_close(udp_socket_t *s){ if(!s)return; if(s->fd!=INVALID_SOCKET) closesocket(s->fd); free(s); }
int udp_recv(udp_socket_t *s, uint8_t *buf, size_t n, udp_addr_t *from) {
    if(!s) return -1;
    struct sockaddr_storage ss; int sl = sizeof ss;
    int r = recvfrom(s->fd, (char*)buf, (int)n, 0, (struct sockaddr*)&ss, &sl);
    if (r == SOCKET_ERROR) { return (WSAGetLastError()==WSAEWOULDBLOCK) ? 0 : -1; }
    if (from){ if((size_t)sl>sizeof from->bytes) sl=(int)sizeof from->bytes;
        memcpy(from->bytes,&ss,sl); from->len=(unsigned)sl; }
    return r;
}
int udp_send(udp_socket_t *s, const uint8_t *buf, size_t n, const udp_addr_t *to) {
    if(!s||!to||to->len==0) return -1;
    int w = sendto(s->fd,(const char*)buf,(int)n,0,(const struct sockaddr*)to->bytes,(int)to->len);
    return (w==SOCKET_ERROR)?-1:w;
}
```

- [ ] **Step 2: CMake — per-OS UDP source + Winsock link + CTest**

In `CMakeLists.txt`:
- Add to the `if(WIN32)` BRIDGE branch: `list(APPEND BRIDGE_SOURCES src/udp_socket_win32.c)`; else branch: `src/udp_socket_unix.c`. Also add `src/frontend_kmbox.c`, `src/frontend_vcom.c`, `src/input_core.c`, `src/kmbox_codec.c`, `src/selector.c` to the common list.
- Link Winsock: `if(WIN32) target_link_libraries(hurra-bridge PRIVATE ws2_32) endif()`
- Add tests at end:
  ```cmake
  enable_testing()
  add_executable(kmbox_codec_test tests/kmbox_codec_test.c src/kmbox_codec.c)
  target_include_directories(kmbox_codec_test PRIVATE src)
  add_test(NAME kmbox_codec COMMAND kmbox_codec_test)

  add_executable(input_core_test tests/input_core_test.c)
  target_include_directories(input_core_test PRIVATE src)
  add_test(NAME input_core COMMAND input_core_test)

  add_executable(ui_util_test tests/ui_util_test.c)
  target_include_directories(ui_util_test PRIVATE src)
  add_test(NAME ui_util COMMAND ui_util_test)

  if(NOT WIN32)
    add_executable(kmbox_loopback_test tests/kmbox_loopback_test.c
        src/frontend_kmbox.c src/kmbox_codec.c src/udp_socket_unix.c)
    target_include_directories(kmbox_loopback_test PRIVATE src include)
    add_test(NAME kmbox_loopback COMMAND kmbox_loopback_test)
  endif()
  ```

- [ ] **Step 3: Build + run full test suite**

Run: `cmake -S . -B build && cmake --build build && (cd build && ctest --output-on-failure)`
Expected: all tests pass.

- [ ] **Step 4: Add a CI test step**

In `.github/workflows/ci.yml`, after the `Build` step (line ~31), add:

```yaml
      - name: Test
        run: ctest --test-dir build --output-on-failure -C Release
```

- [ ] **Step 5: Commit**

```bash
git add src/udp_socket_win32.c CMakeLists.txt .github/workflows/ci.yml
git commit -m "feat: Windows UDP socket + CTest wiring + CI test step"
```

---

## Task 12: Docs — README + usage/help

**Files:**
- Modify: `README.md`
- Modify: `src/bridge.c` (usage text)

- [ ] **Step 1: Update `usage()` in bridge.c**

Add the KMBox flags and a note that an endpoint is chosen interactively at start:

```c
"  --km-port N          UDP port for the KMBox Net endpoint. Default 16896.\n"
"  --km-bind ADDR       Address to bind the KMBox Net UDP socket. Default 0.0.0.0.\n"
"  --km-mac HEX         4-byte MAC clients must present on connect (0 = any).\n"
```

- [ ] **Step 2: Add a README section**

Add a "Endpoints" section explaining: at startup the bridge shows a menu (VCOM / KMBox Net); a TTY is required (breaking change for piped/CI use); for KMBox Net, point client tools at the host IP + `--km-port`, with the MAC matching `--km-mac`. Document which KMBox commands work now vs. are `pending firmware` (auto/bezier move, monitor, mask).

- [ ] **Step 3: Build (usage compiles) and eyeball README**

Run: `cmake --build build 2>&1 | tail -3`
Expected: builds.

- [ ] **Step 4: Commit**

```bash
git add README.md src/bridge.c
git commit -m "docs: document KMBox Net endpoint, selector, and pending-firmware commands"
```

---

## Notes for the implementer

- **Keep VCOM behaviour byte-identical.** Task 7 is a mechanical move of existing `bridge.c` logic into `frontend_vcom.c`; do not rewrite reply formats or the `__diag__` handler.
- **No host-side emulation.** Auto/bezier/monitor/mask only ACK + log until the firmware frames (reserved in Task 1) ship in Hurra-v2. Do not approximate them.
- **`hurra_mo()` already exists** in `hurra.h`/`hurra.c` — call it, don't redefine.
- **Default `--km-port` constant:** pick one value, define it once in `bridge.c` (e.g. `#define KM_DEFAULT_PORT 16896`), and reference it in usage + README. Do not invent a "stock" KMBox port — the bridge owns its default.

## Firmware dependency (separate Hurra-v2 plan, not this repo)

1. `MOUSE_MOVE_DUR 0x1B` — duration-stepped move via humanize filter.
2. `MOUSE_MOVE_BEZIER 0x1C` — cubic Bézier over duration.
3. `CB_PHYS 0x77` + `TLM_PHYS_* 0x86/0x87/0x88` — physical-only telemetry tap before inject merge.
4. `PHYS_MASK 0x68` — enforce `g_lock_mask`/`g_masked_keys` in `kmbox_merge_report()`.

When these land, extend `frontend_kmbox.c` to send the new frames (via new `hurra_*` wrappers) for automove/bezier/mask, and subscribe `TLM_PHYS_*` to relay monitor replies to the cached client address.
