/*
 * hurra.c — Hurra protocol host adapter (client side).
 *
 * One TinyFrame instance per client (TF_MASTER). Outgoing bytes go via
 * TF_WriteImpl → serial_write. Incoming bytes are fed via TF_Accept inside
 * hurra_poll(). Reply correlation uses a small request_slot[] table; telemetry
 * dispatch uses a parallel tlm_slot[] table with registered callbacks.
 * Locking: one recursive mutex per client (see hurra_mutex_init below).
 * Portability: pthread on POSIX, CRITICAL_SECTION on Windows.
 */

#include "hurra.h"
#include "serial.h"
#include "TF_Config.h"
#include "TinyFrame.h"

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#ifdef _WIN32
#  define WIN32_LEAN_AND_MEAN
#  include <windows.h>
   typedef CRITICAL_SECTION    hurra_mutex_t;
   typedef CONDITION_VARIABLE  hurra_cond_t;
#  define hurra_mutex_init(m)    InitializeCriticalSection(m)
#  define hurra_mutex_destroy(m) DeleteCriticalSection(m)
#  define hurra_mutex_lock(m)    EnterCriticalSection(m)
#  define hurra_mutex_unlock(m)  LeaveCriticalSection(m)
#  define hurra_cond_init(c)     InitializeConditionVariable(c)
#  define hurra_cond_destroy(c)  ((void)0)
#  define hurra_cond_signal(c)   WakeConditionVariable(c)
#else
#  include <pthread.h>
#  include <errno.h>
   typedef pthread_mutex_t hurra_mutex_t;
   typedef pthread_cond_t  hurra_cond_t;
   /* RECURSIVE so the same thread can re-lock. hurra_poll() holds the mutex
    * across TF_Accept(), which synchronously dispatches into reply_listener /
    * generic_listener — both of which re-lock the mutex. A non-recursive mutex
    * deadlocks there the instant the firmware sends a frame. (Windows
    * CRITICAL_SECTION is already recursive, so this matches that behavior.) */
   static inline int hurra_mutex_init_recursive(pthread_mutex_t *m) {
       pthread_mutexattr_t attr;
       pthread_mutexattr_init(&attr);
       pthread_mutexattr_settype(&attr, PTHREAD_MUTEX_RECURSIVE);
       int rc = pthread_mutex_init(m, &attr);
       pthread_mutexattr_destroy(&attr);
       return rc;
   }
#  define hurra_mutex_init(m)    hurra_mutex_init_recursive(m)
#  define hurra_mutex_destroy(m) pthread_mutex_destroy(m)
#  define hurra_mutex_lock(m)    pthread_mutex_lock(m)
#  define hurra_mutex_unlock(m)  pthread_mutex_unlock(m)
#  define hurra_cond_init(c)     pthread_cond_init((c), NULL)
#  define hurra_cond_destroy(c)  pthread_cond_destroy(c)
#  define hurra_cond_signal(c)   pthread_cond_signal(c)
#endif

/* Sleep helper used by send_request's poll loop. */
static void hurra_sleep_us(unsigned us) {
#ifdef _WIN32
    Sleep(us / 1000 + (us % 1000 ? 1 : 0));
#else
    struct timespec ts;
    ts.tv_sec  = us / 1000000;
    ts.tv_nsec = (long)(us % 1000000) * 1000L;
    nanosleep(&ts, NULL);
#endif
}

/* Monotonic ms clock. */
static uint64_t hurra_now_ms(void) {
#ifdef _WIN32
    return (uint64_t)GetTickCount64();
#else
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000ULL + (uint64_t)ts.tv_nsec / 1000000ULL;
#endif
}

#define HURRA_REQ_SLOTS  TF_MAX_ID_LST  /* must equal TinyFrame ID listener cap */
#define HURRA_TLM_SLOTS  16
#define HURRA_REPLY_BUF  256

typedef struct {
    bool          in_use;
    bool          completed;
    TF_ID         frame_id;
    uint16_t      reply_len;
    uint8_t       reply[HURRA_REPLY_BUF];
    hurra_cond_t  cond;
} request_slot_t;

typedef struct {
    uint8_t              type;
    hurra_telemetry_cb   cb;
    void                *user;
} tlm_slot_t;

/* TX batch buffer: comfortably above the CH343B FS bulk MPS (64 bytes). */
#define HURRA_TX_BUF_CAP  256

struct hurra_client {
    serial_port_t   *port;
    TinyFrame        tf;
    hurra_mutex_t    mu;

    request_slot_t   req[HURRA_REQ_SLOTS];
    tlm_slot_t       tlm[HURRA_TLM_SLOTS];

    /* tx_batch_target == 0 → immediate-flush mode. Guarded by mu. */
    size_t           tx_batch_target;
    size_t           tx_buf_len;
    uint8_t          tx_buf[HURRA_TX_BUF_CAP];

};

/* Drain tx_buf to the serial port. Caller must hold c->mu. */
static int flush_tx_locked(hurra_client_t *c) {
    if (c->tx_buf_len == 0) return 0;
    size_t off = 0;
    while (off < c->tx_buf_len) {
        int w = serial_write(c->port, c->tx_buf + off, c->tx_buf_len - off);
        if (w < 0) {
            fprintf(stderr, "flush_tx: serial_write returned -1\n");
            fflush(stderr);
            c->tx_buf_len = 0;
            return -1;
        }
        if (w == 0) { hurra_sleep_us(100); continue; }
        off += (size_t)w;
    }
    c->tx_buf_len = 0;
    return 0;
}

/* TF_WriteImpl has no userdata pointer; resolve the client via a thread-local
 * set around every TF_Send/TF_Query call. */
#ifdef _WIN32
__declspec(thread) static hurra_client_t *tls_active_client = NULL;
#else
static __thread hurra_client_t *tls_active_client = NULL;
#endif

void TF_WriteImpl(TinyFrame *tf, const uint8_t *buf, uint32_t len) {
    (void)tf;
    hurra_client_t *c = tls_active_client;
    if (!c || !c->port) return;

    /* Immediate-flush mode. */
    if (c->tx_batch_target == 0) {
        size_t off = 0;
        while (off < len) {
            int w = serial_write(c->port, buf + off, len - off);
            if (w < 0) {
                fprintf(stderr, "TF_WriteImpl: serial_write returned -1\n");
                fflush(stderr);
                return;
            }
            if (w == 0) { hurra_sleep_us(100); continue; }
            off += (size_t)w;
        }
        return;
    }

    /* Batched mode: frames larger than the buffer go direct after flushing. */
    if (len > sizeof(c->tx_buf)) {
        (void)flush_tx_locked(c);
        size_t off = 0;
        while (off < len) {
            int w = serial_write(c->port, buf + off, len - off);
            if (w < 0) return;
            if (w == 0) { hurra_sleep_us(100); continue; }
            off += (size_t)w;
        }
        return;
    }

    /* Flush if this frame would exceed the target, so the next USB transfer
     * contains it intact. */
    if (c->tx_buf_len + len > c->tx_batch_target) {
        (void)flush_tx_locked(c);
    }
    /* Defensive: also catch overflow against the hard buffer cap. */
    if (c->tx_buf_len + len > sizeof(c->tx_buf)) {
        (void)flush_tx_locked(c);
    }
    memcpy(c->tx_buf + c->tx_buf_len, buf, len);
    c->tx_buf_len += len;

    /* Flush at or past the target for a clean MPS-sized USB transfer. */
    if (c->tx_buf_len >= c->tx_batch_target) {
        (void)flush_tx_locked(c);
    }
}

void hurra_set_tx_batch(hurra_client_t *c, size_t batch_bytes) {
    if (!c) return;
    hurra_mutex_lock(&c->mu);
    /* Drain queued bytes before changing policy. */
    (void)flush_tx_locked(c);
    if (batch_bytes > sizeof(c->tx_buf)) batch_bytes = sizeof(c->tx_buf);
    c->tx_batch_target = batch_bytes;
    hurra_mutex_unlock(&c->mu);
}

int hurra_flush(hurra_client_t *c) {
    if (!c) return -1;
    hurra_mutex_lock(&c->mu);
    int rc = flush_tx_locked(c);
    hurra_mutex_unlock(&c->mu);
    return rc;
}

/* ── Reply correlation ────────────────────────────────────────────────────── */

/* Find the request slot for a frame ID. Caller must hold mu. */
static request_slot_t *find_slot_by_id(hurra_client_t *c, TF_ID id) {
    for (size_t i = 0; i < HURRA_REQ_SLOTS; i++) {
        if (c->req[i].in_use && c->req[i].frame_id == id)
            return &c->req[i];
    }
    return NULL;
}

/* ID listener: copies reply into slot, signals the waiting caller. */
static TF_Result reply_listener(TinyFrame *tf, TF_Msg *msg) {
    (void)tf;
    hurra_client_t *c = tls_active_client;
    if (!c) return TF_CLOSE;
    hurra_mutex_lock(&c->mu);
    request_slot_t *slot = find_slot_by_id(c, msg->frame_id);
    if (slot && !slot->completed) {
        uint16_t n = msg->len;
        if (n > HURRA_REPLY_BUF) n = HURRA_REPLY_BUF;
        if (n > 0) memcpy(slot->reply, msg->data, n);
        slot->reply_len = n;
        slot->completed = true;
        hurra_cond_signal(&slot->cond);
    }
    hurra_mutex_unlock(&c->mu);
    return TF_CLOSE;   /* one-shot */
}

/* Generic listener: dispatches TLM_* frames to subscribed callbacks.
 * Invokes the callback without the lock to avoid re-entry deadlocks. */
static TF_Result generic_listener(TinyFrame *tf, TF_Msg *msg) {
    (void)tf;
    hurra_client_t *c = tls_active_client;
    if (!c) return TF_STAY;

    hurra_telemetry_cb cb = NULL;
    void *user = NULL;
    hurra_mutex_lock(&c->mu);
    for (size_t i = 0; i < HURRA_TLM_SLOTS; i++) {
        if (c->tlm[i].cb && c->tlm[i].type == msg->type) {
            cb   = c->tlm[i].cb;
            user = c->tlm[i].user;
            break;
        }
    }
    hurra_mutex_unlock(&c->mu);
    if (cb) cb(msg->type, msg->data, msg->len, user);
    return TF_STAY;
}

/* Send a one-way frame. */
static int send_oneway(hurra_client_t *c, uint8_t type,
                       const uint8_t *payload, uint16_t len) {
    if (!c) return -1;
    hurra_mutex_lock(&c->mu);
    tls_active_client = c;
    TF_Msg msg;
    TF_ClearMsg(&msg);
    msg.type = type;
    msg.data = payload;
    msg.len  = len;
    bool ok = TF_Send(&c->tf, &msg);
    tls_active_client = NULL;
    hurra_mutex_unlock(&c->mu);
    return ok ? 0 : -1;
}

/* Send a request, wait up to timeout_ms for the reply. Returns reply length
 * or -1. Pass out=NULL/out_cap=0 to discard reply payload. */
static int send_request(hurra_client_t *c, uint8_t type,
                        const uint8_t *payload, uint16_t len,
                        uint8_t *out, size_t out_cap,
                        int timeout_ms) {
    if (!c) return -1;

    hurra_mutex_lock(&c->mu);

    /* Find a free slot. Bail if all are in use. */
    request_slot_t *slot = NULL;
    for (size_t i = 0; i < HURRA_REQ_SLOTS; i++) {
        if (!c->req[i].in_use) { slot = &c->req[i]; break; }
    }
    if (!slot) {
        hurra_mutex_unlock(&c->mu);
        return -1;
    }
    slot->in_use    = true;
    slot->completed = false;
    slot->reply_len = 0;

    tls_active_client = c;

    TF_Msg msg;
    TF_ClearMsg(&msg);
    msg.type = type;
    msg.data = payload;
    msg.len  = len;

    /* TF_Query attaches a one-shot ID listener and sends. Timeout managed here. */
    bool ok = TF_Query(&c->tf, &msg, reply_listener, NULL, 0);
    if (!ok) {
        slot->in_use = false;
        tls_active_client = NULL;
        hurra_mutex_unlock(&c->mu);
        return -1;
    }
    slot->frame_id = msg.frame_id;
    /* Must flush before blocking: a half-full TX buffer here is a deadlock. */
    (void)flush_tx_locked(c);
    tls_active_client = NULL;

    /* Poll loop: release lock, pump hurra_poll, re-check completed, repeat.
     * Can't block on a condvar directly because hurra_poll() must run to
     * feed RX bytes into TF_Accept. */
    uint64_t deadline = hurra_now_ms() + (uint64_t)(timeout_ms > 0 ? timeout_ms : 0);
    hurra_mutex_unlock(&c->mu);

    int rc = -1;
    for (;;) {
        int drained = hurra_poll(c);
        (void)drained;

        hurra_mutex_lock(&c->mu);
        bool done = slot->completed;
        uint16_t reply_len = slot->reply_len;
        if (done) {
            if (out && out_cap > 0) {
                uint16_t n = reply_len < out_cap ? reply_len : (uint16_t)out_cap;
                memcpy(out, slot->reply, n);
            }
            rc = (int)reply_len;
            slot->in_use = false;
            hurra_mutex_unlock(&c->mu);
            break;
        }
        hurra_mutex_unlock(&c->mu);

        if (hurra_now_ms() >= deadline) {
            hurra_mutex_lock(&c->mu);
            TF_RemoveIdListener(&c->tf, slot->frame_id);
            slot->in_use = false;
            hurra_mutex_unlock(&c->mu);
            rc = -1;
            break;
        }
        hurra_sleep_us(200);
    }
    return rc;
}

/* ── Lifecycle ───────────────────────────────────────────────────────────── */

hurra_client_t *hurra_open(const char *port, uint32_t baud) {
    hurra_client_t *c = (hurra_client_t *)calloc(1, sizeof(*c));
    if (!c) return NULL;

    c->port = serial_open(port, baud);
    if (!c->port) { free(c); return NULL; }

    hurra_mutex_init(&c->mu);
    for (size_t i = 0; i < HURRA_REQ_SLOTS; i++)
        hurra_cond_init(&c->req[i].cond);

    if (!TF_InitStatic(&c->tf, TF_MASTER)) {
        serial_close(c->port);
        free(c);
        return NULL;
    }
    tls_active_client = c;
    TF_AddGenericListener(&c->tf, generic_listener);
    tls_active_client = NULL;

    return c;
}

void hurra_close(hurra_client_t *c) {
    if (!c) return;
    /* Drain any buffered TX before closing the port. */
    if (c->port) {
        hurra_mutex_lock(&c->mu);
        (void)flush_tx_locked(c);
        hurra_mutex_unlock(&c->mu);
        serial_close(c->port);
    }
    for (size_t i = 0; i < HURRA_REQ_SLOTS; i++)
        hurra_cond_destroy(&c->req[i].cond);
    hurra_mutex_destroy(&c->mu);
    free(c);
}

int hurra_poll(hurra_client_t *c) {
    if (!c) return -1;
    uint8_t buf[512];
    int total = 0;
    for (;;) {
        int n = serial_read(c->port, buf, sizeof(buf));
        if (n < 0) return -1;
        if (n == 0) break;
        hurra_mutex_lock(&c->mu);
        tls_active_client = c;
        TF_Accept(&c->tf, buf, (uint32_t)n);
        TF_Tick(&c->tf);
        tls_active_client = NULL;
        hurra_mutex_unlock(&c->mu);
        total += n;
        if (n < (int)sizeof(buf)) break;
    }
    return total;
}

/* ── Helpers: pack/unpack little-endian ints ─────────────────────────────── */

static void put_i16le(uint8_t *p, int16_t v) {
    p[0] = (uint8_t)(v & 0xFF);
    p[1] = (uint8_t)((v >> 8) & 0xFF);
}
static int16_t get_i16le(const uint8_t *p) {
    return (int16_t)((uint16_t)p[0] | ((uint16_t)p[1] << 8));
}
static void put_u32le(uint8_t *p, uint32_t v) {
    p[0] = (uint8_t)(v & 0xFF);
    p[1] = (uint8_t)((v >> 8) & 0xFF);
    p[2] = (uint8_t)((v >> 16) & 0xFF);
    p[3] = (uint8_t)((v >> 24) & 0xFF);
}
static uint32_t get_u32le(const uint8_t *p) {
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) |
           ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

/* ── Hot path (oneway) ───────────────────────────────────────────────────── */

int hurra_move(hurra_client_t *c, int16_t dx, int16_t dy) {
    uint8_t p[4]; put_i16le(&p[0], dx); put_i16le(&p[2], dy);
    return send_oneway(c, HURRA_TYPE_MOUSE_MOVE, p, sizeof(p));
}
int hurra_move_smooth(hurra_client_t *c, int16_t dx, int16_t dy) {
    uint8_t p[4]; put_i16le(&p[0], dx); put_i16le(&p[2], dy);
    return send_oneway(c, HURRA_TYPE_MOUSE_MOVE_SMOOTH, p, sizeof(p));
}
int hurra_silent_move(hurra_client_t *c, int16_t dx, int16_t dy) {
    uint8_t p[4]; put_i16le(&p[0], dx); put_i16le(&p[2], dy);
    return send_oneway(c, HURRA_TYPE_MOUSE_SILENT_MOVE, p, sizeof(p));
}
int hurra_human(hurra_client_t *c, uint8_t level) {
    return send_oneway(c, HURRA_TYPE_HUMAN, &level, sizeof(level));
}
int hurra_mo(hurra_client_t *c, uint8_t buttons,
             int16_t dx, int16_t dy, int8_t wheel, int8_t pan, int8_t tilt) {
    uint8_t p[8];
    p[0] = buttons;
    put_i16le(&p[1], dx);
    put_i16le(&p[3], dy);
    p[5] = (uint8_t)wheel;
    p[6] = (uint8_t)pan;
    p[7] = (uint8_t)tilt;
    return send_oneway(c, HURRA_TYPE_MOUSE_MO, p, sizeof(p));
}
int hurra_click(hurra_client_t *c, uint8_t button, uint8_t count, uint8_t delay_ms) {
    uint8_t p[3] = { button, count, delay_ms };
    return send_oneway(c, HURRA_TYPE_MOUSE_CLICK, p, sizeof(p));
}
int hurra_wheel(hurra_client_t *c, int8_t ticks) {
    uint8_t p[1] = { (uint8_t)ticks };
    return send_oneway(c, HURRA_TYPE_MOUSE_WHEEL, p, sizeof(p));
}
int hurra_button(hurra_client_t *c, uint8_t mask, uint8_t state) {
    /* mask: 0=L, 1=R, 2=M, 3=S1, 4=S2 */
    uint8_t type;
    switch (mask) {
        case 0: type = HURRA_TYPE_BTN_LEFT;   break;
        case 1: type = HURRA_TYPE_BTN_RIGHT;  break;
        case 2: type = HURRA_TYPE_BTN_MIDDLE; break;
        case 3: type = HURRA_TYPE_BTN_SIDE1;  break;
        case 4: type = HURRA_TYPE_BTN_SIDE2;  break;
        default: return -1;
    }
    uint8_t p[1] = { state ? 1 : 0 };
    return send_oneway(c, type, p, sizeof(p));
}

int hurra_button_get(hurra_client_t *c, uint8_t button, bool *out, int timeout_ms) {
    /* button: 0=L,1=R,2=M,3=S1,4=S2. Empty-payload BTN_* → firmware replies state byte. */
    if (!out) return -1;
    uint8_t type;
    switch (button) {
        case 0: type = HURRA_TYPE_BTN_LEFT;   break;
        case 1: type = HURRA_TYPE_BTN_RIGHT;  break;
        case 2: type = HURRA_TYPE_BTN_MIDDLE; break;
        case 3: type = HURRA_TYPE_BTN_SIDE1;  break;
        case 4: type = HURRA_TYPE_BTN_SIDE2;  break;
        default: return -1;
    }
    uint8_t buf[2];
    int n = send_request(c, type, NULL, 0, buf, sizeof(buf), timeout_ms);
    if (n < 1) return -1;
    *out = buf[0] != 0;
    return 0;
}

/* ── Request/reply ───────────────────────────────────────────────────────── */

int hurra_version(hurra_client_t *c, char *out, size_t outsz, int timeout_ms) {
    if (!out || outsz == 0) return -1;
    uint8_t buf[HURRA_REPLY_BUF];
    int n = send_request(c, HURRA_TYPE_VERSION, NULL, 0,
                         buf, sizeof(buf), timeout_ms);
    if (n < 0) return -1;
    size_t copy = (size_t)n < (outsz - 1) ? (size_t)n : (outsz - 1);
    memcpy(out, buf, copy);
    out[copy] = '\0';
    return 0;
}

int hurra_ping(hurra_client_t *c, uint64_t *rtt_us, int timeout_ms) {
    uint8_t req[4];
    /* Nonce: use low-order ms so successive pings differ. Echoed back. */
    uint32_t nonce = (uint32_t)hurra_now_ms();
    put_u32le(req, nonce);

#ifdef _WIN32
    LARGE_INTEGER freq, t0, t1;
    QueryPerformanceFrequency(&freq);
    QueryPerformanceCounter(&t0);
#else
    struct timespec t0, t1;
    clock_gettime(CLOCK_MONOTONIC, &t0);
#endif

    uint8_t buf[16];
    int n = send_request(c, HURRA_TYPE_PING, req, sizeof(req),
                         buf, sizeof(buf), timeout_ms);
    if (n < 0) return -1;

#ifdef _WIN32
    QueryPerformanceCounter(&t1);
    uint64_t us = (uint64_t)((t1.QuadPart - t0.QuadPart) * 1000000ULL / freq.QuadPart);
#else
    clock_gettime(CLOCK_MONOTONIC, &t1);
    uint64_t us = (uint64_t)(t1.tv_sec - t0.tv_sec) * 1000000ULL +
                  (uint64_t)((t1.tv_nsec - t0.tv_nsec) / 1000);
#endif
    if (rtt_us) *rtt_us = us;
    return 0;
}

int hurra_stats(hurra_client_t *c, hurra_stats_t *out, int timeout_ms) {
    if (!out) return -1;
    uint8_t buf[64];
    int n = send_request(c, HURRA_TYPE_STATS, NULL, 0,
                         buf, sizeof(buf), timeout_ms);
    if (n < 36) return -1;
    out->uptime_ms       = get_u32le(&buf[0]);
    out->rx_frames_ok    = get_u32le(&buf[4]);
    out->head_crc_err    = get_u32le(&buf[8]);
    out->payload_crc_err = get_u32le(&buf[12]);
    out->id_gap_total    = get_u32le(&buf[16]);
    out->idle_resync     = get_u32le(&buf[20]);
    out->rx_drv_overrun  = get_u32le(&buf[24]);
    out->tx_ring_skip    = get_u32le(&buf[28]);
    out->payload_invalid = get_u32le(&buf[32]);
    return 0;
}

int hurra_getpos(hurra_client_t *c, int16_t *x, int16_t *y, int timeout_ms) {
    if (!x || !y) return -1;
    uint8_t buf[8];
    int n = send_request(c, HURRA_TYPE_MOUSE_GETPOS, NULL, 0,
                         buf, sizeof(buf), timeout_ms);
    if (n < 4) return -1;
    *x = get_i16le(&buf[0]);
    *y = get_i16le(&buf[2]);
    return 0;
}

int hurra_get_baud(hurra_client_t *c, uint32_t *baud, int timeout_ms) {
    if (!baud) return -1;
    uint8_t buf[8];
    int n = send_request(c, HURRA_TYPE_BAUD, NULL, 0,
                         buf, sizeof(buf), timeout_ms);
    if (n < 4) return -1;
    *baud = get_u32le(&buf[0]);
    return 0;
}

int hurra_set_baud(hurra_client_t *c, uint32_t baud, int timeout_ms) {
    uint8_t req[4]; put_u32le(req, baud);
    uint8_t buf[8];
    int n = send_request(c, HURRA_TYPE_BAUD, req, sizeof(req),
                         buf, sizeof(buf), timeout_ms);
    return (n >= 4) ? 0 : -1;
}

/* ── Admin ───────────────────────────────────────────────────────────────── */

int hurra_init_remote(hurra_client_t *c) {
    return send_oneway(c, HURRA_TYPE_INIT, NULL, 0);
}
int hurra_reboot(hurra_client_t *c) {
    return send_oneway(c, HURRA_TYPE_REBOOT, NULL, 0);
}
int hurra_screen_set(hurra_client_t *c, int16_t w, int16_t h) {
    uint8_t p[4]; put_i16le(&p[0], w); put_i16le(&p[2], h);
    return send_oneway(c, HURRA_TYPE_SCREEN, p, sizeof(p));
}
int hurra_screen_get(hurra_client_t *c, int16_t *w, int16_t *h, int timeout_ms) {
    if (!w || !h) return -1;
    uint8_t buf[8];
    int n = send_request(c, HURRA_TYPE_SCREEN, NULL, 0,
                         buf, sizeof(buf), timeout_ms);
    if (n < 4) return -1;
    *w = get_i16le(&buf[0]);
    *h = get_i16le(&buf[2]);
    return 0;
}

/* ── Keyboard ────────────────────────────────────────────────────────────── */

int hurra_kb_down (hurra_client_t *c, uint8_t hid) {
    return send_oneway(c, HURRA_TYPE_KB_DOWN, &hid, 1);
}
int hurra_kb_up   (hurra_client_t *c, uint8_t hid) {
    return send_oneway(c, HURRA_TYPE_KB_UP, &hid, 1);
}
int hurra_kb_press(hurra_client_t *c, uint8_t hid, uint8_t hold_ms, uint8_t rand_ms) {
    uint8_t p[3] = { hid, hold_ms, rand_ms };
    return send_oneway(c, HURRA_TYPE_KB_PRESS, p, sizeof(p));
}
int hurra_kb_isdown(hurra_client_t *c, uint8_t hid, bool *out, int timeout_ms) {
    if (!out) return -1;
    uint8_t buf[2];
    int n = send_request(c, HURRA_TYPE_KB_ISDOWN, &hid, 1,
                         buf, sizeof(buf), timeout_ms);
    if (n < 1) return -1;
    *out = buf[0] != 0;
    return 0;
}
int hurra_kb_mask(hurra_client_t *c, uint8_t hid, uint8_t state) {
    uint8_t p[2] = { hid, state };
    return send_oneway(c, HURRA_TYPE_KB_MASK, p, sizeof(p));
}
int hurra_kb_string(hurra_client_t *c, const char *s) {
    if (!s) return -1;
    size_t n = strlen(s);
    if (n > 240) n = 240;        /* spec §3: max 240 chars */
    return send_oneway(c, HURRA_TYPE_KB_STRING, (const uint8_t *)s, (uint16_t)n);
}
int hurra_kb_multidown(hurra_client_t *c, const uint8_t *keys, size_t n) {
    if (!keys || n == 0 || n > 6) return -1;
    return send_oneway(c, HURRA_TYPE_KB_MULTIDOWN, keys, (uint16_t)n);
}
int hurra_kb_multiup(hurra_client_t *c, const uint8_t *keys, size_t n) {
    if (!keys || n == 0 || n > 6) return -1;
    return send_oneway(c, HURRA_TYPE_KB_MULTIUP, keys, (uint16_t)n);
}
int hurra_kb_multipress(hurra_client_t *c, const uint8_t *keys, size_t n) {
    if (!keys || n == 0 || n > 6) return -1;
    return send_oneway(c, HURRA_TYPE_KB_MULTIPRESS, keys, (uint16_t)n);
}

/* ── Locks ───────────────────────────────────────────────────────────────── */

static int lock_type_for(const char *name, uint8_t *type) {
    if      (!strcmp(name, "ml"))  *type = HURRA_TYPE_LOCK_ML;
    else if (!strcmp(name, "mr"))  *type = HURRA_TYPE_LOCK_MR;
    else if (!strcmp(name, "mm"))  *type = HURRA_TYPE_LOCK_MM;
    else if (!strcmp(name, "ms1")) *type = HURRA_TYPE_LOCK_MS1;
    else if (!strcmp(name, "ms2")) *type = HURRA_TYPE_LOCK_MS2;
    else if (!strcmp(name, "mx"))  *type = HURRA_TYPE_LOCK_MX;
    else if (!strcmp(name, "my"))  *type = HURRA_TYPE_LOCK_MY;
    else return -1;
    return 0;
}

int hurra_lock(hurra_client_t *c, const char *name, int *state_inout,
               int timeout_ms) {
    if (!name || !state_inout) return -1;
    uint8_t type;
    if (lock_type_for(name, &type) != 0) return -1;

    if (*state_inout >= 0) {
        /* Set: one-way; firmware doesn't reply. */
        uint8_t p[1] = { *state_inout ? 1 : 0 };
        return send_oneway(c, type, p, sizeof(p));
    }
    /* Get: empty payload; reply is u8 state. */
    uint8_t buf[2];
    int n = send_request(c, type, NULL, 0, buf, sizeof(buf), timeout_ms);
    if (n < 1) return -1;
    *state_inout = buf[0];
    return 0;
}

int hurra_catch_xy(hurra_client_t *c, uint16_t dur_ms,
                   int32_t *dx_accum, int32_t *dy_accum) {
    uint8_t req[2];
    req[0] = (uint8_t)(dur_ms & 0xFF);
    req[1] = (uint8_t)((dur_ms >> 8) & 0xFF);
    uint8_t buf[16];
    /* Budget dur_ms + 1 s slack for the deferred reply. */
    int n = send_request(c, HURRA_TYPE_CATCH_XY, req, sizeof(req),
                         buf, sizeof(buf), (int)dur_ms + 1000);
    if (n < 8) return -1;
    if (dx_accum) *dx_accum = (int32_t)get_u32le(&buf[0]);
    if (dy_accum) *dy_accum = (int32_t)get_u32le(&buf[4]);
    return 0;
}

/* ── Telemetry streams ───────────────────────────────────────────────────── */

int hurra_stream_axis(hurra_client_t *c, uint8_t mode, uint8_t period_ms) {
    uint8_t p[2] = { mode, period_ms };
    return send_oneway(c, HURRA_TYPE_STREAM_AXIS, p, sizeof(p));
}
int hurra_stream_buttons(hurra_client_t *c, uint8_t mode, uint8_t period_ms) {
    uint8_t p[2] = { mode, period_ms };
    return send_oneway(c, HURRA_TYPE_STREAM_BTN, p, sizeof(p));
}
int hurra_stream_mouse(hurra_client_t *c, uint8_t mode, uint8_t period_ms) {
    uint8_t p[2] = { mode, period_ms };
    return send_oneway(c, HURRA_TYPE_STREAM_MOUSE, p, sizeof(p));
}
int hurra_stream_keyboard(hurra_client_t *c, uint8_t mode, uint8_t period_ms) {
    uint8_t p[2] = { mode, period_ms };
    return send_oneway(c, HURRA_TYPE_STREAM_KB, p, sizeof(p));
}

int hurra_cb_buttons(hurra_client_t *c, uint8_t enable) {
    uint8_t p[1] = { enable ? 1 : 0 };
    return send_oneway(c, HURRA_TYPE_CB_BUTTONS, p, sizeof(p));
}
int hurra_cb_axes(hurra_client_t *c, uint8_t enable) {
    uint8_t p[1] = { enable ? 1 : 0 };
    return send_oneway(c, HURRA_TYPE_CB_AXES, p, sizeof(p));
}
int hurra_cb_keys(hurra_client_t *c, uint8_t enable) {
    uint8_t p[1] = { enable ? 1 : 0 };
    return send_oneway(c, HURRA_TYPE_CB_KEYS, p, sizeof(p));
}

int hurra_on_telemetry(hurra_client_t *c, uint8_t type,
                       hurra_telemetry_cb handler, void *user) {
    if (!c) return -1;
    hurra_mutex_lock(&c->mu);

    /* Replace existing entry for same type, or take the first empty slot.
     * If handler is NULL, clear matching entry. */
    int target = -1;
    int empty  = -1;
    for (size_t i = 0; i < HURRA_TLM_SLOTS; i++) {
        if (c->tlm[i].cb && c->tlm[i].type == type) { target = (int)i; break; }
        if (!c->tlm[i].cb && empty < 0) empty = (int)i;
    }
    int rc = 0;
    if (handler) {
        int slot = target >= 0 ? target : empty;
        if (slot < 0) rc = -1;
        else {
            c->tlm[slot].type = type;
            c->tlm[slot].cb   = handler;
            c->tlm[slot].user = user;
        }
    } else if (target >= 0) {
        c->tlm[target].cb   = NULL;
        c->tlm[target].user = NULL;
    }
    hurra_mutex_unlock(&c->mu);
    return rc;
}
