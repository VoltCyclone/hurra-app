/*
 * serial_win32.c — Win32 serial backend.
 *
 * Uses CreateFileA + DCB. DCB.BaudRate accepts arbitrary integer values, so
 * 4_000_000 works as long as the underlying USB-serial driver supports it
 * (the WCH CH343 driver does; some generic drivers cap at 1.5 Mbps).
 */
#include "serial.h"

#ifdef _WIN32

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define WIN32_LEAN_AND_MEAN
#include <windows.h>

struct serial_port {
    HANDLE h;
};

static const char *prefix_path(const char *path, char *buf, size_t buflen) {
    /* COM10+ requires the "\\.\COMx" syntax. Always prefix to be safe. */
    if (path && path[0] == '\\') return path;
    if (path && (path[0] == 'C' || path[0] == 'c') &&
        (path[1] == 'O' || path[1] == 'o') &&
        (path[2] == 'M' || path[2] == 'm')) {
        snprintf(buf, buflen, "\\\\.\\%s", path);
        return buf;
    }
    return path;
}

serial_port_t *serial_open(const char *path, uint32_t baud) {
    char fullpath[64];
    const char *p = prefix_path(path, fullpath, sizeof(fullpath));

    HANDLE h = CreateFileA(p, GENERIC_READ | GENERIC_WRITE,
                           0, NULL, OPEN_EXISTING, 0, NULL);
    if (h == INVALID_HANDLE_VALUE) {
        fprintf(stderr, "serial_open(%s): CreateFileA failed (err=%lu)\n",
                p, (unsigned long)GetLastError());
        return NULL;
    }

    DCB dcb;
    memset(&dcb, 0, sizeof(dcb));
    dcb.DCBlength = sizeof(dcb);
    if (!GetCommState(h, &dcb)) {
        CloseHandle(h);
        return NULL;
    }
    dcb.BaudRate     = baud;
    dcb.ByteSize     = 8;
    dcb.Parity       = NOPARITY;
    dcb.StopBits     = ONESTOPBIT;
    dcb.fBinary      = TRUE;
    dcb.fOutxCtsFlow = FALSE;
    dcb.fOutxDsrFlow = FALSE;
    dcb.fDtrControl  = DTR_CONTROL_DISABLE;
    dcb.fRtsControl  = RTS_CONTROL_DISABLE;
    dcb.fInX         = FALSE;
    dcb.fOutX        = FALSE;
    dcb.fNull        = FALSE;
    dcb.fAbortOnError= FALSE;
    if (!SetCommState(h, &dcb)) {
        fprintf(stderr, "serial_open(%s): SetCommState failed (err=%lu)\n",
                p, (unsigned long)GetLastError());
        CloseHandle(h);
        return NULL;
    }

    /* Non-blocking reads: all timeouts to zero except the MaxBetween value
     * which signals "return immediately with whatever is available." */
    COMMTIMEOUTS to;
    memset(&to, 0, sizeof(to));
    to.ReadIntervalTimeout         = MAXDWORD;
    to.ReadTotalTimeoutConstant    = 0;
    to.ReadTotalTimeoutMultiplier  = 0;
    to.WriteTotalTimeoutConstant   = 0;
    to.WriteTotalTimeoutMultiplier = 0;
    if (!SetCommTimeouts(h, &to)) {
        CloseHandle(h);
        return NULL;
    }
    PurgeComm(h, PURGE_RXCLEAR | PURGE_TXCLEAR);

    serial_port_t *s = (serial_port_t *)calloc(1, sizeof(*s));
    if (!s) { CloseHandle(h); return NULL; }
    s->h = h;
    return s;
}

void serial_close(serial_port_t *s) {
    if (!s) return;
    if (s->h && s->h != INVALID_HANDLE_VALUE) CloseHandle(s->h);
    free(s);
}

int serial_write(serial_port_t *s, const uint8_t *buf, size_t n) {
    if (!s || s->h == INVALID_HANDLE_VALUE) return -1;
    DWORD written = 0;
    if (!WriteFile(s->h, buf, (DWORD)n, &written, NULL)) return -1;
    return (int)written;
}

int serial_read(serial_port_t *s, uint8_t *buf, size_t n) {
    if (!s || s->h == INVALID_HANDLE_VALUE) return -1;
    DWORD got = 0;
    if (!ReadFile(s->h, buf, (DWORD)n, &got, NULL)) return -1;
    return (int)got;
}

#endif /* _WIN32 */
