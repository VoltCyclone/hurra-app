/*
 * serial_unix.c — termios serial backend for macOS and Linux.
 *
 * macOS: standard tcsetattr() configures everything except the baud rate;
 *        non-standard rates (e.g. 4 Mbps) are then applied with
 *        ioctl(fd, IOSSIOSPEED, &speed). The header <IOKit/serial/ioss.h>
 *        defines IOSSIOSPEED = 0x80045402, but we hardcode the value to
 *        avoid an IOKit dependency at build time.
 *
 * Linux: <termios.h>'s cfsetispeed/cfsetospeed only understands the
 *        standard B<NNN> constants, none of which cover 4_000_000. We
 *        instead populate a `struct termios2` (declared in
 *        <asm/termbits.h>) with c_cflag |= BOTHER and c_i/ospeed = baud,
 *        then push it with ioctl(fd, TCSETS2, &t2). Mixing
 *        <asm/termbits.h> with <termios.h> in the same TU produces type
 *        clashes, so the Linux path bypasses <termios.h> entirely.
 */
#include "serial.h"

#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <unistd.h>

#if defined(__linux__)
#  include <asm/termbits.h>      /* struct termios2, BOTHER, TCSETS2 */
#else
#  include <termios.h>
#endif

#if defined(__APPLE__)
#  ifndef IOSSIOSPEED
#    define IOSSIOSPEED _IOW('T', 2, speed_t)   /* 0x80045402 */
#  endif
#endif

struct serial_port {
    int fd;
};

#if defined(__linux__)
static int configure_linux(int fd, uint32_t baud) {
    struct termios2 t2;
    memset(&t2, 0, sizeof(t2));
    if (ioctl(fd, TCGETS2, &t2) < 0) return -1;

    t2.c_cflag &= ~CBAUD;
    t2.c_cflag |= BOTHER | CS8 | CREAD | CLOCAL;
    t2.c_cflag &= ~(PARENB | CSTOPB);   /* 8-N-1 */
    t2.c_cflag &= ~CRTSCTS;             /* no HW flow control */

    t2.c_iflag = 0;                     /* raw */
    t2.c_oflag = 0;
    t2.c_lflag = 0;

    t2.c_ispeed = baud;
    t2.c_ospeed = baud;

    /* VMIN=0, VTIME=0 → non-blocking read returns immediately. */
    t2.c_cc[VMIN]  = 0;
    t2.c_cc[VTIME] = 0;

    if (ioctl(fd, TCSETS2, &t2) < 0) return -1;
    return 0;
}
#else
static int configure_termios(int fd, uint32_t baud) {
    struct termios tio;
    if (tcgetattr(fd, &tio) < 0) return -1;
    cfmakeraw(&tio);
    tio.c_cflag |= (CLOCAL | CREAD | CS8);
    tio.c_cflag &= ~(PARENB | CSTOPB | CRTSCTS);
    tio.c_cc[VMIN]  = 0;
    tio.c_cc[VTIME] = 0;
    /* B9600 placeholder; IOSSIOSPEED below sets the real rate. */
    cfsetispeed(&tio, B9600);
    cfsetospeed(&tio, B9600);
    if (tcsetattr(fd, TCSANOW, &tio) < 0) return -1;

#  if defined(__APPLE__)
    speed_t speed = (speed_t)baud;
    if (ioctl(fd, IOSSIOSPEED, &speed) < 0) return -1;
#  else
    /* Generic POSIX fallback (rarely used; macOS/Linux are handled above). */
    (void)baud;
#  endif
    return 0;
}
#endif

serial_port_t *serial_open(const char *path, uint32_t baud) {
    int fd = open(path, O_RDWR | O_NOCTTY | O_NONBLOCK);
    if (fd < 0) {
        return NULL;
    }

#if defined(__linux__)
    if (configure_linux(fd, baud) != 0) {
        close(fd);
        return NULL;
    }
#else
    if (configure_termios(fd, baud) != 0) {
        close(fd);
        return NULL;
    }
#endif

    serial_port_t *s = (serial_port_t *)calloc(1, sizeof(*s));
    if (!s) { close(fd); return NULL; }
    s->fd = fd;
    return s;
}

void serial_close(serial_port_t *s) {
    if (!s) return;
    if (s->fd >= 0) close(s->fd);
    free(s);
}

int serial_write(serial_port_t *s, const uint8_t *buf, size_t n) {
    if (!s || s->fd < 0) return -1;
    size_t off = 0;
    while (off < n) {
        ssize_t w = write(s->fd, buf + off, n - off);
        if (w < 0) {
            if (errno == EAGAIN || errno == EWOULDBLOCK) {
                /* Kernel buffer full; caller will retry. */
                return (int)off;
            }
            if (errno == EINTR) continue;
            return -1;
        }
        off += (size_t)w;
    }
    return (int)off;
}

int serial_read(serial_port_t *s, uint8_t *buf, size_t n) {
    if (!s || s->fd < 0) return -1;
    ssize_t r = read(s->fd, buf, n);
    if (r < 0) {
        if (errno == EAGAIN || errno == EWOULDBLOCK) return 0;
        if (errno == EINTR) return 0;
        return -1;
    }
    return (int)r;
}
