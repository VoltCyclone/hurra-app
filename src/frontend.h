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
    /* Borrowed human-readable description for the banner (e.g. the PTY path). */
    const char *(*describe)(frontend_t *fe);
};

#ifdef __cplusplus
}
#endif

#endif /* HURRA_FRONTEND_H */
