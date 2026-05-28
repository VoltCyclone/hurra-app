/*
 * virtual_port.h — abstraction over a virtual COM port the bridge owns.
 *
 * Unix:    posix_openpt master-side fd; the slave path (e.g. /dev/ttys004)
 *          is what clients open. Optionally symlinked to a stable path.
 * Windows: a com0com kernel virtual COM (e.g. CNCA0) opened with CreateFileA.
 *
 * The bridge reads/writes the master/COM end; user tools speak Ferrum ASCII
 * on the opposite side.
 */
#ifndef HURRA_VIRTUAL_PORT_H
#define HURRA_VIRTUAL_PORT_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct vp_port vp_port_t;

/* Open the virtual port.
 *
 * Unix: arg is ignored except for diagnostics; the slave path is allocated
 *       by the kernel and accessible via vp_slave_path().
 *       link_path (nullable) is a symlink that will point at the slave path;
 *       pass NULL to skip the symlink.
 * Win32: arg is the com0com COM name (e.g. "CNCA0" or "COM7"); link_path is
 *        ignored.
 *
 * Returns NULL on failure; check errno / GetLastError().
 */
vp_port_t  *vp_open(const char *arg, const char *link_path);
void        vp_close(vp_port_t *vp);

/* The slave path (Unix) or COM name (Win32). Borrowed pointer, valid until
 * vp_close. May return NULL on Win32 if the implementation chose not to
 * stash the path. */
const char *vp_slave_path(vp_port_t *vp);

/* Non-blocking read of up to n bytes. Returns 0 when no data is available,
 * the number of bytes read on success, or -1 on a hard error. */
int         vp_read (vp_port_t *vp, uint8_t *buf, size_t n);

/* Write n bytes. Returns the number written (>=0), -1 on error. May write
 * fewer bytes than requested if the kernel buffer is full. */
int         vp_write(vp_port_t *vp, const uint8_t *buf, size_t n);

#ifdef __cplusplus
}
#endif

#endif /* HURRA_VIRTUAL_PORT_H */
