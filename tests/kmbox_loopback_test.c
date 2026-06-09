/* KMBox loopback: bind frontend, send connect+mouse_move from a client socket,
 * assert ACK returns and the mock sink saw the move.
 *   cc -std=c99 -D_DEFAULT_SOURCE -Wall -Wextra -Isrc -o build/kmbox_loopback_test \
 *      tests/kmbox_loopback_test.c src/frontend_kmbox.c src/kmbox_codec.c src/udp_socket_unix.c
 */
#include "frontend_kmbox.h"
#include "kmbox_codec.h"
#include <arpa/inet.h>
#include <netinet/in.h>
#include <stdio.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <unistd.h>

static int g_fail = 0;
#define CHECK(c) do{ if(!(c)){printf("FAIL %s:%d %s\n",__FILE__,__LINE__,#c);g_fail=1;} }while(0)

/* Spin-poll the non-blocking frontend until it processes a datagram (returns >0)
 * or 2 000 000 iterations elapse (a hard timeout to avoid hanging in CI).
 * Returns the last poll() return value. */
static int poll_until_work(frontend_t *fe) {
    for (int i = 0; i < 2000000; i++) {
        int r = fe->poll(fe);
        if (r != 0) return r;
    }
    return 0;
}

static int g_moves=0, g_last_x=0, g_last_y=0;
static void s_move(input_sink_t*s,int32_t dx,int32_t dy){(void)s;g_moves++;g_last_x=dx;g_last_y=dy;}
static void s_noop_btn(input_sink_t*s,int b,int d){(void)s;(void)b;(void)d;}
static void s_noop_wheel(input_sink_t*s,int32_t t){(void)s;(void)t;}
static void s_noop_all(input_sink_t*s,uint8_t b,int32_t x,int32_t y,int32_t w){(void)s;(void)b;(void)x;(void)y;(void)w;}
static void s_noop_kb(input_sink_t*s,uint8_t m,const uint8_t*k,int n){(void)s;(void)m;(void)k;(void)n;}
static void s_noop_reboot(input_sink_t*s){(void)s;}

static void put_u32le(uint8_t*p,uint32_t v){p[0]=(uint8_t)v;p[1]=(uint8_t)(v>>8);p[2]=(uint8_t)(v>>16);p[3]=(uint8_t)(v>>24);}
static void put_i32le(uint8_t*p,int32_t v){put_u32le(p,(uint32_t)v);}

int main(void){
    const uint16_t PORT = 39222;
    input_sink_t sink; memset(&sink,0,sizeof sink);
    sink.move=s_move; sink.button=s_noop_btn; sink.wheel=s_noop_wheel;
    sink.mouse_all=s_noop_all; sink.kb_report=s_noop_kb; sink.reboot=s_noop_reboot;

    frontend_t fe; memset(&fe,0,sizeof fe);
    CHECK(frontend_kmbox_open(&fe,&sink,"127.0.0.1",PORT,0)==0);

    int cli = socket(AF_INET,SOCK_DGRAM,0);
    struct timeval tv = { 2, 0 };  /* 2s recv timeout so a missing ACK fails, not hangs */
    setsockopt(cli, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof tv);
    struct sockaddr_in srv; memset(&srv,0,sizeof srv);
    srv.sin_family=AF_INET; srv.sin_port=htons(PORT);
    inet_pton(AF_INET,"127.0.0.1",&srv.sin_addr);

    /* connect */
    uint8_t pkt[KM_HEAD_SIZE+KM_MOUSE_PAYLOAD_SIZE]; memset(pkt,0,sizeof pkt);
    put_u32le(&pkt[12],KM_CMD_CONNECT);
    sendto(cli,pkt,KM_HEAD_SIZE,0,(struct sockaddr*)&srv,sizeof srv);
    CHECK(poll_until_work(&fe) > 0);
    uint8_t ack[64]; ssize_t r = recv(cli,ack,sizeof ack,0);
    CHECK(r==KM_HEAD_SIZE);

    /* mouse_move 100,-50 */
    memset(pkt,0,sizeof pkt);
    put_u32le(&pkt[8],1); put_u32le(&pkt[12],KM_CMD_MOUSE_MOVE);
    put_i32le(&pkt[KM_HEAD_SIZE+4],100); put_i32le(&pkt[KM_HEAD_SIZE+8],-50);
    sendto(cli,pkt,sizeof pkt,0,(struct sockaddr*)&srv,sizeof srv);
    CHECK(poll_until_work(&fe) > 0);
    r = recv(cli,ack,sizeof ack,0);
    CHECK(r==KM_HEAD_SIZE);
    CHECK(g_moves==1 && g_last_x==100 && g_last_y==-50);

    close(cli);
    fe.close(&fe);
    if(g_fail){printf("TESTS FAILED\n");return 1;}
    printf("ALL TESTS PASSED\n");
    return 0;
}
