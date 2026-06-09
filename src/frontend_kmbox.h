#ifndef HURRA_FRONTEND_KMBOX_H
#define HURRA_FRONTEND_KMBOX_H
#include "frontend.h"
#include <stdint.h>
#ifdef __cplusplus
extern "C" {
#endif
/* Open the KMBox Net frontend: bind UDP on bind_addr:port, accept clients
 * presenting `mac` in connect (mac==0 means accept any). The sink is borrowed.
 * Returns 0 on success; -1 on bind failure (errno set). */
int frontend_kmbox_open(frontend_t *out, input_sink_t *sink,
                        const char *bind_addr, uint16_t port, uint32_t mac);
#ifdef __cplusplus
}
#endif
#endif
