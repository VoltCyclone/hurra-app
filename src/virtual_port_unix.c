/*
 * virtual_port_unix.c — PTY-backed virtual port.
 *
 * Opens a pseudo-terminal pair via posix_openpt/grantpt/unlockpt/ptsname.
 * The master fd is what the bridge reads/writes; the slave path is what
 * Ferrum-speaking clients open. A stable symlink is optionally created
 * pointing at the slave path so clients can configure a fixed target.
 */
#define _XOPEN_SOURCE 600
#define _DEFAULT_SOURCE 1
/* cfmakeraw + posix_openpt: on macOS these live behind _DARWIN_C_SOURCE,
 * on Linux behind _DEFAULT_SOURCE / _BSD_SOURCE. */
#define _DARWIN_C_SOURCE 1
#define _BSD_SOURCE 1

#include "virtual_port.h"

#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/types.h>
#include <termios.h>
#include <unistd.h>

struct vp_port {
    int   master_fd;
    char *slave_path;   /* heap copy of ptsname() result */
    char *link_path;    /* heap copy of the symlink we created (or NULL) */
};

static int set_nonblock(int fd) {
    int flags = fcntl(fd, F_GETFL, 0);
    if (flags < 0) return -1;
    if (fcntl(fd, F_SETFL, flags | O_NONBLOCK) < 0) return -1;
    return 0;
}

vp_port_t *vp_open(const char *arg, const char *link_path) {
    (void)arg;

    int mfd = posix_openpt(O_RDWR | O_NOCTTY);
    if (mfd < 0) return NULL;

    if (grantpt(mfd) < 0) { int e = errno; close(mfd); errno = e; return NULL; }
    if (unlockpt(mfd) < 0) { int e = errno; close(mfd); errno = e; return NULL; }

    /* ptsname() returns static storage on some platforms; copy it. */
    const char *sp = ptsname(mfd);
    if (!sp) { int e = errno; close(mfd); errno = e; return NULL; }

    char *slave_copy = strdup(sp);
    if (!slave_copy) { close(mfd); errno = ENOMEM; return NULL; }

    /* Raw mode: prevents line discipline echo/translation between the two ends. */
    struct termios tio;
    if (tcgetattr(mfd, &tio) == 0) {
        cfmakeraw(&tio);
        tio.c_cc[VMIN]  = 0;
        tio.c_cc[VTIME] = 0;
        tcsetattr(mfd, TCSANOW, &tio);
    }
    set_nonblock(mfd);

    /* Open the slave once so the PTY stays connected before any client attaches;
     * on macOS reads return EIO if no one has the slave open. */
    int sfd = open(slave_copy, O_RDWR | O_NOCTTY);
    if (sfd >= 0) {
        /* Apply raw mode to the slave so clients see byte-exact IO. */
        struct termios sti;
        if (tcgetattr(sfd, &sti) == 0) {
            cfmakeraw(&sti);
            tcsetattr(sfd, TCSANOW, &sti);
        }
        /* Intentionally leak sfd: keeps PTY alive across client open/close cycles. */
        (void)sfd;
    }

    char *link_copy = NULL;
    if (link_path && link_path[0]) {
        /* Best-effort: remove stale symlink, create new one. Non-fatal on failure. */
        (void)unlink(link_path);
        if (symlink(slave_copy, link_path) == 0) {
            link_copy = strdup(link_path);
        } else {
            fprintf(stderr, "vp: symlink(%s -> %s) failed: %s\n",
                    link_path, slave_copy, strerror(errno));
        }
    }

    vp_port_t *vp = (vp_port_t *)calloc(1, sizeof(*vp));
    if (!vp) {
        close(mfd);
        free(slave_copy);
        free(link_copy);
        errno = ENOMEM;
        return NULL;
    }
    vp->master_fd  = mfd;
    vp->slave_path = slave_copy;
    vp->link_path  = link_copy;
    return vp;
}

void vp_close(vp_port_t *vp) {
    if (!vp) return;
    if (vp->master_fd >= 0) close(vp->master_fd);
    if (vp->link_path) {
        (void)unlink(vp->link_path);
        free(vp->link_path);
    }
    free(vp->slave_path);
    free(vp);
}

const char *vp_slave_path(vp_port_t *vp) {
    return vp ? vp->slave_path : NULL;
}

int vp_read(vp_port_t *vp, uint8_t *buf, size_t n) {
    if (!vp || vp->master_fd < 0) return -1;
    ssize_t r = read(vp->master_fd, buf, n);
    if (r < 0) {
        if (errno == EAGAIN || errno == EWOULDBLOCK) return 0;
        /* macOS: EIO means slave closed; treat as no data so bridge stays alive. */
        if (errno == EIO) return 0;
        return -1;
    }
    return (int)r;
}

int vp_write(vp_port_t *vp, const uint8_t *buf, size_t n) {
    if (!vp || vp->master_fd < 0) return -1;
    ssize_t w = write(vp->master_fd, buf, n);
    if (w < 0) {
        if (errno == EAGAIN || errno == EWOULDBLOCK) return 0;
        if (errno == EIO) return 0;   /* slave not attached yet */
        return -1;
    }
    return (int)w;
}
