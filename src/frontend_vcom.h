#ifndef HURRA_FRONTEND_VCOM_H
#define HURRA_FRONTEND_VCOM_H
#include <stdbool.h>
#include "frontend.h"
struct hurra_client;
#ifdef __cplusplus
extern "C" {
#endif
/* Optional health hook: called once per Ferrum `version` query with the result
 * of the live firmware re-probe (true=responding). Lets the host keep link
 * health fresh without the frontend knowing about its stats counters.
 * Same borrowed-pointer pattern as input_sink_t::move_count. */
typedef void (*vcom_health_cb)(void *user, bool ok);

/* Open the VCOM frontend. vp_arg is the com0com COM name (Windows) or NULL
 * (Unix); link_path is the Unix symlink path (or NULL). The sink and hc are
 * borrowed. health_cb may be NULL. Returns 0 on success, -1 on failure
 * (vp_open failed). */
int frontend_vcom_open(frontend_t *out, input_sink_t *sink,
                       struct hurra_client *hc,
                       const char *vp_arg, const char *link_path,
                       int request_timeout_ms,
                       vcom_health_cb health_cb, void *health_user);
/* The PTY slave path / COM name, for the banner (borrowed, may be NULL). */
const char *frontend_vcom_slave_path(frontend_t *fe);
#ifdef __cplusplus
}
#endif
#endif
