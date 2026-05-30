/*
 * hello.c — minimal Hurra adapter smoke test.
 *
 * Connects to a Hurra firmware over the given (or default) serial port,
 * prints the firmware version + a ping RTT, then nudges the mouse 50 px
 * right and closes.
 *
 * Usage: hello [/dev/cu.usbmodem01] [baud]
 */
#include <stdio.h>
#include <stdlib.h>

#include "hurra.h"

int main(int argc, char **argv) {
    const char *port = argc > 1 ? argv[1] : "/dev/cu.usbmodem01";
    uint32_t baud = argc > 2 ? (uint32_t)strtoul(argv[2], NULL, 10) : 4000000;

    hurra_client_t *c = hurra_open(port, baud);
    if (!c) {
        fprintf(stderr, "open failed: %s\n", port);
        return 1;
    }

    char version[64];
    if (hurra_version(c, version, sizeof(version), 1000) == 0) {
        printf("version: %s\n", version);
    } else {
        fprintf(stderr, "version: timed out\n");
    }

    uint64_t rtt_us = 0;
    if (hurra_ping(c, &rtt_us, 1000) == 0) {
        printf("ping: %llu us\n", (unsigned long long)rtt_us);
    } else {
        fprintf(stderr, "ping: timed out\n");
    }

    hurra_move(c, 50, 0);

    hurra_close(c);
    return 0;
}
