#include "udp_socket.h"
#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

struct udp_socket { int fd; };

static int set_nonblock(int fd) {
    int fl = fcntl(fd, F_GETFL, 0);
    if (fl < 0) return -1;
    return fcntl(fd, F_SETFL, fl | O_NONBLOCK);
}

udp_socket_t *udp_open(const char *bind_addr, uint16_t port) {
    int fd = socket(AF_INET, SOCK_DGRAM, 0);
    if (fd < 0) return NULL;

    struct sockaddr_in sa;
    memset(&sa, 0, sizeof sa);
    sa.sin_family = AF_INET;
    sa.sin_port   = htons(port);
    if (!bind_addr || !*bind_addr) sa.sin_addr.s_addr = htonl(INADDR_ANY);
    else if (inet_pton(AF_INET, bind_addr, &sa.sin_addr) != 1) {
        close(fd); errno = EINVAL; return NULL;
    }
    if (bind(fd, (struct sockaddr *)&sa, sizeof sa) < 0) {
        int e = errno; close(fd); errno = e; return NULL;
    }
    if (set_nonblock(fd) < 0) { int e = errno; close(fd); errno = e; return NULL; }

    udp_socket_t *s = calloc(1, sizeof *s);
    if (!s) { close(fd); errno = ENOMEM; return NULL; }
    s->fd = fd;
    return s;
}

void udp_close(udp_socket_t *s) {
    if (!s) return;
    if (s->fd >= 0) close(s->fd);
    free(s);
}

int udp_recv(udp_socket_t *s, uint8_t *buf, size_t n, udp_addr_t *from) {
    if (!s) return -1;
    struct sockaddr_storage ss;
    socklen_t sl = sizeof ss;
    ssize_t r = recvfrom(s->fd, buf, n, 0, (struct sockaddr *)&ss, &sl);
    if (r < 0) {
        if (errno == EAGAIN || errno == EWOULDBLOCK) return 0;
        return -1;
    }
    if (from) {
        if (sl > sizeof from->bytes) sl = sizeof from->bytes;
        memcpy(from->bytes, &ss, sl);
        from->len = (unsigned int)sl;
    }
    return (int)r;
}

int udp_send(udp_socket_t *s, const uint8_t *buf, size_t n, const udp_addr_t *to) {
    if (!s || !to || to->len == 0) return -1;
    ssize_t w = sendto(s->fd, buf, n, 0,
                       (const struct sockaddr *)to->bytes, (socklen_t)to->len);
    return (w < 0) ? -1 : (int)w;
}
