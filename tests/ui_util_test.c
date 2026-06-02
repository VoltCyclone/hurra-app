/* Ad-hoc unit tests for src/ui_util.h pure helpers. Compile with:
 *   cc -std=c99 -Wall -Wextra -Isrc -o build/ui_util_test tests/ui_util_test.c
 */
#include "ui_util.h"
#include <stdio.h>
#include <string.h>
#include <errno.h>

static int g_fail = 0;
#define CHECK_STR(expr, want) do {                                        \
    const char *got_ = (expr);                                            \
    if (strcmp(got_, (want)) != 0) {                                      \
        printf("FAIL %s:%d  %s == \"%s\", want \"%s\"\n",                 \
               __FILE__, __LINE__, #expr, got_, (want));                  \
        g_fail = 1;                                                       \
    }                                                                     \
} while (0)

static void test_humanize_baud(void) {
    char buf[32];
    CHECK_STR((ui_humanize_baud(4000000, buf, sizeof buf), buf), "4 Mbaud");
    CHECK_STR((ui_humanize_baud(2000000, buf, sizeof buf), buf), "2 Mbaud");
    CHECK_STR((ui_humanize_baud(115200,  buf, sizeof buf), buf), "115200 baud");
    CHECK_STR((ui_humanize_baud(1500000, buf, sizeof buf), buf), "1500000 baud");
}

static void test_humanize_uptime(void) {
    char buf[32];
    CHECK_STR((ui_humanize_uptime(0,    buf, sizeof buf), buf), "0s");
    CHECK_STR((ui_humanize_uptime(45,   buf, sizeof buf), buf), "45s");
    CHECK_STR((ui_humanize_uptime(84,   buf, sizeof buf), buf), "1m24s");
    CHECK_STR((ui_humanize_uptime(3661, buf, sizeof buf), buf), "1h01m01s");
}

static void test_group_thousands(void) {
    char buf[32];
    CHECK_STR((ui_group_thousands(0,     buf, sizeof buf), buf), "0");
    CHECK_STR((ui_group_thousands(12480, buf, sizeof buf), buf), "12,480");
    CHECK_STR((ui_group_thousands(1000,  buf, sizeof buf), buf), "1,000");
    CHECK_STR((ui_group_thousands(999,   buf, sizeof buf), buf), "999");
    CHECK_STR((ui_group_thousands(1000000, buf, sizeof buf), buf), "1,000,000");
}

static void test_classify_open(void) {
    if (ui_open_category(ENOENT) != UI_OPEN_NOENT) { printf("FAIL ENOENT\n"); g_fail = 1; }
    if (ui_open_category(EACCES) != UI_OPEN_ACCES) { printf("FAIL EACCES\n"); g_fail = 1; }
    if (ui_open_category(EBUSY)  != UI_OPEN_BUSY)  { printf("FAIL EBUSY\n");  g_fail = 1; }
    if (ui_open_category(EIO)    != UI_OPEN_OTHER) { printf("FAIL EIO\n");    g_fail = 1; }
}

static void test_link_health(void) {
    CHECK_STR(ui_link_health(5, 0), "ok");
    CHECK_STR(ui_link_health(0, 3), "dead");
    CHECK_STR(ui_link_health(4, 2), "flapping");
    CHECK_STR(ui_link_health(0, 0), "unknown");
}

static void test_spinner(void) {
    if (ui_spinner_ascii(0) != '|') { printf("FAIL spin0\n"); g_fail = 1; }
    if (ui_spinner_ascii(1) != '/') { printf("FAIL spin1\n"); g_fail = 1; }
    if (ui_spinner_ascii(4) != '|') { printf("FAIL spin4 wrap\n"); g_fail = 1; }
}

int main(void) {
    test_humanize_baud();
    test_humanize_uptime();
    test_group_thousands();
    test_classify_open();
    test_link_health();
    test_spinner();
    if (g_fail) { printf("TESTS FAILED\n"); return 1; }
    printf("ALL TESTS PASSED\n");
    return 0;
}
