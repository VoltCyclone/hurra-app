/*
 * udp_socket.h — minimal non-blocking UDP socket the KMBox frontend owns.
 * Server-side: bind a local port, recvfrom clients, sendto the last client.
 */
#ifndef HURRA_UDP_SOCKET_H
#define HURRA_UDP_SOCKET_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct udp_socket udp_socket_t;

/* Opaque client address (sockaddr_storage under the hood). */
typedef struct { unsigned char bytes[128]; unsigned int len; } udp_addr_t;

/* Bind UDP on bind_addr:port (e.g. "0.0.0.0", 12345). NULL addr -> 0.0.0.0.
 * Returns NULL on failure (errno set). */
udp_socket_t *udp_open(const char *bind_addr, uint16_t port);
void          udp_close(udp_socket_t *s);

/* Non-blocking recv. Returns bytes read (>0), 0 if no datagram waiting,
 * -1 on hard error. Fills *from with the sender address. */
int udp_recv(udp_socket_t *s, uint8_t *buf, size_t n, udp_addr_t *from);

/* Send to a specific address. Returns bytes sent or -1. */
int udp_send(udp_socket_t *s, const uint8_t *buf, size_t n, const udp_addr_t *to);

#ifdef __cplusplus
}
#endif

#endif /* HURRA_UDP_SOCKET_H */
