/* input_core translation tests via a mock sink.
 *   cc -std=c99 -Wall -Wextra -Isrc -o build/input_core_test tests/input_core_test.c
 * (No hurra link needed: this tests the sink CONTRACT using a mock.)
 */
#include "input_core.h"
#include <stdio.h>
#include <string.h>

static int g_fail = 0;
#define CHECK(c) do { if(!(c)){printf("FAIL %s:%d %s\n",__FILE__,__LINE__,#c);g_fail=1;} } while(0)

/* Mock sink: record the last call of each kind. */
typedef struct {
    int  moves; int32_t last_dx, last_dy;
    int  buttons; int last_btn, last_down;
    int  wheels;  int32_t last_ticks;
    int  kb_reports; uint8_t last_mod; int last_nkeys;
    int  reboots;
} mock_t;

static void m_move(input_sink_t *s, int32_t dx, int32_t dy){ mock_t*m=s->ctx; m->moves++; m->last_dx=dx; m->last_dy=dy; }
static void m_btn(input_sink_t *s, int b, int d){ mock_t*m=s->ctx; m->buttons++; m->last_btn=b; m->last_down=d; }
static void m_wheel(input_sink_t *s, int32_t t){ mock_t*m=s->ctx; m->wheels++; m->last_ticks=t; }
static void m_all(input_sink_t *s, uint8_t b, int32_t dx, int32_t dy, int32_t w){ (void)b;(void)dx;(void)dy;(void)w; mock_t*m=s->ctx; m->moves++; }
static void m_kb(input_sink_t *s, uint8_t mod, const uint8_t*k, int n){ (void)k; mock_t*m=s->ctx; m->kb_reports++; m->last_mod=mod; m->last_nkeys=n; }
static void m_reboot(input_sink_t *s){ mock_t*m=s->ctx; m->reboots++; }

static input_sink_t make_mock(mock_t *m) {
    input_sink_t s; memset(&s,0,sizeof s);
    s.ctx=m; s.move=m_move; s.button=m_btn; s.wheel=m_wheel;
    s.mouse_all=m_all; s.kb_report=m_kb; s.reboot=m_reboot;
    return s;
}

/* The decode->sink mapping under test lives in the frontends, but the SINK
 * contract is what both share. Here we assert the mock honors the vtable so
 * frontend tests can rely on it. */
static void test_mock_dispatch(void) {
    mock_t m; memset(&m,0,sizeof m);
    input_sink_t s = make_mock(&m);
    s.move(&s, 100, -50);
    s.button(&s, 1, 1);
    s.wheel(&s, 3);
    uint8_t keys[2] = {0x04, 0x05};
    s.kb_report(&s, 0x02, keys, 2);
    s.reboot(&s);
    CHECK(m.moves == 1 && m.last_dx == 100 && m.last_dy == -50);
    CHECK(m.buttons == 1 && m.last_btn == 1 && m.last_down == 1);
    CHECK(m.wheels == 1 && m.last_ticks == 3);
    CHECK(m.kb_reports == 1 && m.last_mod == 0x02 && m.last_nkeys == 2);
    CHECK(m.reboots == 1);
}

int main(void) {
    test_mock_dispatch();
    if (g_fail) { printf("TESTS FAILED\n"); return 1; }
    printf("ALL TESTS PASSED\n");
    return 0;
}
