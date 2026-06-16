/*
 * serial_enum_win32.c — enumerate COM ports via SetupAPI.
 *
 * Walks GUID_DEVCLASS_PORTS for present devices, reads each port's COM name
 * (Device Parameters\PortName), friendly name, and hardware ID, and classifies
 * it with serial_classify_hwid. Any per-device failure skips that device; a
 * whole-enumeration failure returns 0 so callers fall back to explicit flags.
 */
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <setupapi.h>
#include <devguid.h>

#include "serial_enum.h"

#include <stdio.h>
#include <string.h>

/* Read the "PortName" value (e.g. "COM5") from a device's hardware reg key. */
static int read_port_name(HDEVINFO set, SP_DEVINFO_DATA *dev,
                          char *out, size_t cap) {
    HKEY k = SetupDiOpenDevRegKey(set, dev, DICS_FLAG_GLOBAL, 0,
                                  DIREG_DEV, KEY_READ);
    if (k == INVALID_HANDLE_VALUE) return 0;
    DWORD type = 0, len = (DWORD)cap;
    LONG r = RegQueryValueExA(k, "PortName", NULL, &type,
                              (LPBYTE)out, &len);
    RegCloseKey(k);
    if (r != ERROR_SUCCESS || type != REG_SZ) return 0;
    /* out[cap-1]='\0' covers both a non-terminated REG_SZ value and exact-fill
     * truncation — fine here since COM port names are only a few chars. */
    out[cap - 1] = '\0';
    return out[0] != '\0';
}

/* Read a string device property. SPDRP_FRIENDLYNAME is REG_SZ; SPDRP_HARDWAREID
 * is REG_MULTI_SZ (a \0-separated list). In both cases the first C-string in the
 * buffer is what we want, and serial_classify_hwid only needs that first ID
 * (the most-specific one, which carries the VID). Reject other types. */
static int get_str_prop(HDEVINFO set, SP_DEVINFO_DATA *dev, DWORD prop,
                        char *out, size_t cap) {
    DWORD type = 0;
    out[0] = '\0';
    if (!SetupDiGetDeviceRegistryPropertyA(set, dev, prop, &type,
                                           (PBYTE)out, (DWORD)cap, NULL))
        return 0;
    if (type != REG_SZ && type != REG_MULTI_SZ) { out[0] = '\0'; return 0; }
    out[cap - 1] = '\0';   /* guarantee termination of the first string */
    return out[0] != '\0';
}

size_t serial_enum(serial_cand_t *out, size_t max) {
    if (!out || max == 0) return 0;
    HDEVINFO set = SetupDiGetClassDevsA(&GUID_DEVCLASS_PORTS, NULL, NULL,
                                        DIGCF_PRESENT);
    if (set == INVALID_HANDLE_VALUE) return 0;

    size_t n = 0;
    SP_DEVINFO_DATA dev;
    dev.cbSize = sizeof(dev);
    for (DWORD i = 0; n < max &&
                      SetupDiEnumDeviceInfo(set, i, &dev); i++) {
        char name[32];
        if (!read_port_name(set, &dev, name, sizeof name)) continue;

        char hwid[256];
        get_str_prop(set, &dev, SPDRP_HARDWAREID, hwid, sizeof hwid);

        char friendly[128];
        if (!get_str_prop(set, &dev, SPDRP_FRIENDLYNAME,
                          friendly, sizeof friendly))
            snprintf(friendly, sizeof friendly, "%s", name);

        snprintf(out[n].name, sizeof out[n].name, "%s", name);
        snprintf(out[n].friendly, sizeof out[n].friendly, "%s", friendly);
        out[n].klass = serial_classify_hwid(hwid);
        n++;
    }
    SetupDiDestroyDeviceInfoList(set);
    return n;
}
