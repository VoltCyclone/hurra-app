#include "frontend_kmbox.h"
#include "kmbox_codec.h"
#include "udp_socket.h"
#include <stdbool.h>
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
    bool          debug;      /* HURRA_KM_DEBUG: trace every datagram to stderr */
} kmbox_t;

static void warn_once(bool *flag, const char *cmd) {
    if (!*flag) { fprintf(stderr, "kmbox: pending firmware: %s\n", cmd); *flag = true; }
}

/* Human-readable name for a KMBox command code (for diagnostics). */
static const char *km_cmd_name(uint32_t cmd) {
    switch (cmd) {
        case KM_CMD_CONNECT:        return "connect";
        case KM_CMD_MOUSE_MOVE:     return "mouse_move";
        case KM_CMD_MOUSE_LEFT:     return "mouse_left";
        case KM_CMD_MOUSE_RIGHT:    return "mouse_right";
        case KM_CMD_MOUSE_MIDDLE:   return "mouse_middle";
        case KM_CMD_MOUSE_WHEEL:    return "mouse_wheel";
        case KM_CMD_MOUSE_AUTOMOVE: return "mouse_automove";
        case KM_CMD_BEZIER:         return "bezier";
        case KM_CMD_KEYBOARD_ALL:   return "keyboard_all";
        case KM_CMD_REBOOT:         return "reboot";
        case KM_CMD_MONITOR:        return "monitor";
        case KM_CMD_MASK_MOUSE:     return "mask_mouse";
        case KM_CMD_UNMASK_ALL:     return "unmask_all";
        default:                    return "UNKNOWN";
    }
}

static int km_poll(frontend_t *fe) {
    kmbox_t *k = (kmbox_t *)fe->impl;
    uint8_t buf[256];
    udp_addr_t from;
    int n = udp_recv(k->sock, buf, sizeof buf, &from);
    if (n <= 0) return n;   /* 0 idle, -1 error */

    km_decoded_t d = km_decode(buf, (size_t)n);
    if (k->debug) {
        if (!d.valid)
            fprintf(stderr, "kmbox rx: n=%d  TOO SHORT (need >= %d for header)\n",
                    n, KM_HEAD_SIZE);
        else
            fprintf(stderr, "kmbox rx: n=%d  cmd=0x%08X (%s)  mac=0x%08X  x=%d y=%d btn=%d\n",
                    n, d.head.cmd, km_cmd_name(d.head.cmd), d.head.mac,
                    (int)d.x, (int)d.y, (int)d.button);
    }
    if (!d.valid) { k->rx_bad++; return 1; }

    bool ack = true;
    switch (d.head.cmd) {
        case KM_CMD_CONNECT:
            if (k->mac != 0 && d.head.mac != k->mac) {
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
        case KM_CMD_MOUSE_AUTOMOVE: warn_once(&k->warned_automove, "automove"); break;
        case KM_CMD_BEZIER:         warn_once(&k->warned_bezier,   "bezier");   break;
        case KM_CMD_MONITOR:        warn_once(&k->warned_monitor,  "monitor");  break;
        case KM_CMD_MASK_MOUSE:
        case KM_CMD_UNMASK_ALL:     warn_once(&k->warned_mask,     "mask");     break;
        default:
            if (k->debug)
                fprintf(stderr, "kmbox: unknown cmd 0x%08X dropped (no ACK)\n", d.head.cmd);
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
    { const char *dbg = getenv("HURRA_KM_DEBUG"); k->debug = (dbg && *dbg && *dbg != '0'); }
    snprintf(k->desc, sizeof k->desc, "UDP %s:%u",
             (bind_addr && *bind_addr) ? bind_addr : "0.0.0.0", (unsigned)port);
    out->impl     = k;
    out->poll     = km_poll;
    out->close    = km_close;
    out->describe = km_describe;
    return 0;
}
