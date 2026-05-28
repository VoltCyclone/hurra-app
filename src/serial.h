/*
 * serial.h — minimal cross-platform serial port abstraction.
 *
 * Implemented by serial_unix.c (macOS+Linux termios) and serial_win32.c
 * (Windows CreateFileA + DCB). Configuration is fixed: 8-N-1, no flow
 * control, raw, non-blocking reads. Baud is whatever the caller asks for —
 * each backend uses platform-specific custom-baud APIs (IOSSIOSPEED on
 * macOS, termios2/BOTHER on Linux, DCB.BaudRate on Windows).
 */
#pragma once

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct serial_port serial_port_t;

/* Open and configure the port. Returns NULL on failure; errno is set on
 * POSIX, GetLastError() on Windows. */
serial_port_t *serial_open(const char *path, uint32_t baud);
void           serial_close(serial_port_t *s);

/* Write n bytes. Returns the number written (>=0) on success, -1 on error.
 * May write fewer bytes than requested if the kernel buffer is full. */
int            serial_write(serial_port_t *s, const uint8_t *buf, size_t n);

/* Non-blocking read of up to n bytes. Returns 0 when no data is available,
 * the number of bytes read on success, or -1 on a hard error. */
int            serial_read (serial_port_t *s, uint8_t *buf, size_t n);

#ifdef __cplusplus
}
#endif
