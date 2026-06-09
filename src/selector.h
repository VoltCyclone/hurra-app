#ifndef HURRA_SELECTOR_H
#define HURRA_SELECTOR_H
#ifdef __cplusplus
extern "C" {
#endif
typedef enum { ENDPOINT_VCOM = 1, ENDPOINT_KMBOX = 2 } endpoint_t;
/* Render the endpoint menu to stderr and read a choice from stdin.
 * Returns the chosen endpoint (1 or 2), or -1 if stdin is not a TTY. */
int selector_choose(void);
#ifdef __cplusplus
}
#endif
#endif
