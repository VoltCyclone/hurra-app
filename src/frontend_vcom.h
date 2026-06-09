#ifndef HURRA_FRONTEND_VCOM_H
#define HURRA_FRONTEND_VCOM_H
#include "frontend.h"
struct hurra_client;
#ifdef __cplusplus
extern "C" {
#endif
/* Open the VCOM frontend. vp_arg is the com0com COM name (Windows) or NULL
 * (Unix); link_path is the Unix symlink path (or NULL). The sink and hc are
 * borrowed. Returns 0 on success, -1 on failure (vp_open failed). */
int frontend_vcom_open(frontend_t *out, input_sink_t *sink,
                       struct hurra_client *hc,
                       const char *vp_arg, const char *link_path,
                       int request_timeout_ms);
/* The PTY slave path / COM name, for the banner (borrowed, may be NULL). */
const char *frontend_vcom_slave_path(frontend_t *fe);
#ifdef __cplusplus
}
#endif
#endif
