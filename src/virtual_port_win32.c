/*
 * virtual_port_win32.c — com0com-backed virtual port.
 *
 * The user pre-configures a com0com pair (e.g. CNCA0 ↔ CNCB0) via
 * setupc.exe. The bridge opens one end (CNCA0); their Ferrum-speaking tool
 * opens the other (CNCB0).
 *
 * COM ports with index >= 10 require the "\\.\COMx" prefix on CreateFileA;
 * com0com's CNCA0/CNCB0 names also require it. To keep the code simple we
 * always prepend "\\.\" if it isn't already there.
 */
#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#include "virtual_port.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

struct vp_port {
    HANDLE  h;
    char   *com_name;   /* heap copy of the COM name, for diagnostics */
};

static char *prefix_unc(const char *name) {
    /* Already prefixed? */
    if (name[0] == '\\' && name[1] == '\\' && name[2] == '.' && name[3] == '\\') {
        return _strdup(name);
    }
    size_t n = strlen(name);
    char *out = (char *)malloc(n + 5);
    if (!out) return NULL;
    memcpy(out, "\\\\.\\", 4);
    memcpy(out + 4, name, n + 1);
    return out;
}

vp_port_t *vp_open(const char *arg, const char *link_path) {
    (void)link_path;   /* no symlink concept on Windows */
    if (!arg || !arg[0]) { SetLastError(ERROR_INVALID_PARAMETER); return NULL; }

    char *path = prefix_unc(arg);
    if (!path) { SetLastError(ERROR_NOT_ENOUGH_MEMORY); return NULL; }

    HANDLE h = CreateFileA(path,
                           GENERIC_READ | GENERIC_WRITE,
                           0, NULL, OPEN_EXISTING,
                           FILE_ATTRIBUTE_NORMAL, NULL);
    free(path);
    if (h == INVALID_HANDLE_VALUE) return NULL;

    /* Configure as a raw 8N1 link. com0com doesn't really need this since
     * both ends are kernel-virtual, but it can't hurt and matches what most
     * Ferrum-speaking clients would expect. */
    DCB dcb;
    memset(&dcb, 0, sizeof(dcb));
    dcb.DCBlength = sizeof(dcb);
    if (GetCommState(h, &dcb)) {
        dcb.BaudRate = CBR_115200;
        dcb.ByteSize = 8;
        dcb.Parity   = NOPARITY;
        dcb.StopBits = ONESTOPBIT;
        dcb.fBinary  = TRUE;
        dcb.fParity  = FALSE;
        dcb.fOutxCtsFlow = FALSE;
        dcb.fOutxDsrFlow = FALSE;
        dcb.fDtrControl  = DTR_CONTROL_DISABLE;
        dcb.fRtsControl  = RTS_CONTROL_DISABLE;
        dcb.fOutX = FALSE;
        dcb.fInX  = FALSE;
        SetCommState(h, &dcb);
    }

    /* Non-blocking-ish timeouts: ReadFile returns immediately when no data
     * is queued. */
    COMMTIMEOUTS to;
    memset(&to, 0, sizeof(to));
    to.ReadIntervalTimeout         = MAXDWORD;
    to.ReadTotalTimeoutConstant    = 0;
    to.ReadTotalTimeoutMultiplier  = 0;
    to.WriteTotalTimeoutConstant   = 50;
    to.WriteTotalTimeoutMultiplier = 0;
    SetCommTimeouts(h, &to);

    vp_port_t *vp = (vp_port_t *)calloc(1, sizeof(*vp));
    if (!vp) { CloseHandle(h); SetLastError(ERROR_NOT_ENOUGH_MEMORY); return NULL; }
    vp->h        = h;
    vp->com_name = _strdup(arg);
    return vp;
}

void vp_close(vp_port_t *vp) {
    if (!vp) return;
    if (vp->h && vp->h != INVALID_HANDLE_VALUE) CloseHandle(vp->h);
    free(vp->com_name);
    free(vp);
}

const char *vp_slave_path(vp_port_t *vp) {
    return vp ? vp->com_name : NULL;
}

int vp_read(vp_port_t *vp, uint8_t *buf, size_t n) {
    if (!vp || !vp->h) return -1;
    DWORD got = 0;
    if (!ReadFile(vp->h, buf, (DWORD)n, &got, NULL)) {
        DWORD e = GetLastError();
        if (e == ERROR_IO_PENDING || e == ERROR_TIMEOUT) return 0;
        return -1;
    }
    return (int)got;
}

int vp_write(vp_port_t *vp, const uint8_t *buf, size_t n) {
    if (!vp || !vp->h) return -1;
    DWORD wrote = 0;
    if (!WriteFile(vp->h, buf, (DWORD)n, &wrote, NULL)) {
        DWORD e = GetLastError();
        if (e == ERROR_IO_PENDING || e == ERROR_TIMEOUT) return 0;
        return -1;
    }
    return (int)wrote;
}
