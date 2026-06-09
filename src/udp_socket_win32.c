#include "udp_socket.h"
#define WIN32_LEAN_AND_MEAN
#include <winsock2.h>
#include <ws2tcpip.h>
#include <stdlib.h>
#include <string.h>

struct udp_socket { SOCKET fd; };
static int g_wsa = 0;

udp_socket_t *udp_open(const char *bind_addr, uint16_t port) {
    if (!g_wsa) { WSADATA w; if (WSAStartup(MAKEWORD(2,2), &w) != 0) return NULL; g_wsa = 1; }
    SOCKET fd = socket(AF_INET, SOCK_DGRAM, 0);
    if (fd == INVALID_SOCKET) return NULL;
    struct sockaddr_in sa; memset(&sa, 0, sizeof sa);
    sa.sin_family = AF_INET; sa.sin_port = htons(port);
    if (!bind_addr || !*bind_addr) sa.sin_addr.s_addr = htonl(INADDR_ANY);
    else if (inet_pton(AF_INET, bind_addr, &sa.sin_addr) != 1) { closesocket(fd); return NULL; }
    if (bind(fd, (struct sockaddr*)&sa, sizeof sa) == SOCKET_ERROR) { closesocket(fd); return NULL; }
    u_long nb = 1; ioctlsocket(fd, FIONBIO, &nb);
    udp_socket_t *s = calloc(1, sizeof *s); if (!s) { closesocket(fd); return NULL; }
    s->fd = fd; return s;
}
void udp_close(udp_socket_t *s){ if(!s)return; if(s->fd!=INVALID_SOCKET) closesocket(s->fd); free(s); }
int udp_recv(udp_socket_t *s, uint8_t *buf, size_t n, udp_addr_t *from) {
    if(!s) return -1;
    struct sockaddr_storage ss; int sl = sizeof ss;
    int r = recvfrom(s->fd, (char*)buf, (int)n, 0, (struct sockaddr*)&ss, &sl);
    if (r == SOCKET_ERROR) { return (WSAGetLastError()==WSAEWOULDBLOCK) ? 0 : -1; }
    if (from){ if((size_t)sl>sizeof from->bytes) sl=(int)sizeof from->bytes;
        memcpy(from->bytes,&ss,sl); from->len=(unsigned)sl; }
    return r;
}
int udp_send(udp_socket_t *s, const uint8_t *buf, size_t n, const udp_addr_t *to) {
    if(!s||!to||to->len==0) return -1;
    int w = sendto(s->fd,(const char*)buf,(int)n,0,(const struct sockaddr*)to->bytes,(int)to->len);
    return (w==SOCKET_ERROR)?-1:w;
}
