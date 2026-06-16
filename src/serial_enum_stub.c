/* serial_enum_stub.c — serial_enum for non-Windows builds (no enumeration). */
#include "serial_enum.h"

size_t serial_enum(serial_cand_t *out, size_t max) {
    (void)out; (void)max;
    return 0;   /* Unix uses glob-based discover_devices() in bridge.c instead. */
}
