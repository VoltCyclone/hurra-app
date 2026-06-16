/* Unit tests for src/serial_enum.c classifier. */
#include "serial_enum.h"
#include <stdio.h>

static int g_fail = 0;
#define CHECK_CLASS(hwid, want) do {                                       \
    port_class_t got_ = serial_classify_hwid(hwid);                       \
    if (got_ != (want)) {                                                 \
        printf("FAIL %s:%d  classify(%s) == %d, want %d\n",               \
               __FILE__, __LINE__, (hwid) ? (hwid) : "(null)",           \
               (int)got_, (int)(want));                                   \
        g_fail = 1;                                                       \
    }                                                                     \
} while (0)

static void test_firmware(void) {
    CHECK_CLASS("USB\\VID_1A86&PID_55D3", PORT_FIRMWARE);
    CHECK_CLASS("usb\\vid_1a86&pid_7523", PORT_FIRMWARE);  /* lowercase */
    CHECK_CLASS("FTDIBUS\\VID_1A86+ABC",  PORT_FIRMWARE);
    CHECK_CLASS("VID_1A86\\COM0COM\\CNCA0", PORT_FIRMWARE);  /* firmware wins priority */
}

static void test_com0com(void) {
    CHECK_CLASS("com0com\\port\\CNCA0",   PORT_COM0COM);
    CHECK_CLASS("COM0COM\\PORT\\CNCB0",   PORT_COM0COM);
    CHECK_CLASS("something-CNCA-here",    PORT_COM0COM);
    CHECK_CLASS("something-cncb-here",    PORT_COM0COM);
}

static void test_other(void) {
    CHECK_CLASS("USB\\VID_0403&PID_6001", PORT_OTHER);  /* FTDI */
    CHECK_CLASS("ACPI\\PNP0501",          PORT_OTHER);
    CHECK_CLASS("",                       PORT_OTHER);
    CHECK_CLASS(NULL,                     PORT_OTHER);
}

int main(void) {
    test_firmware();
    test_com0com();
    test_other();
    if (g_fail) { printf("TESTS FAILED\n"); return 1; }
    printf("ALL TESTS PASSED\n");
    return 0;
}
