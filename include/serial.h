/* serial.h - minimal POSIX serial port helper (raw mode). */
#ifndef UHF_SERIAL_H
#define UHF_SERIAL_H

#include <stddef.h>
#include <stdint.h>

/* Open `path` in raw 8N1 mode at `baud` (e.g. 115200).
 * Returns an fd >= 0 on success, or -1 with errno set. */
int serial_open(const char *path, int baud);

/* Close a port opened with serial_open(). */
void serial_close(int fd);

/* Change baud on an already-open port. Returns 0 / -1 (errno set). */
int serial_set_baud(int fd, int baud);

/* Write exactly `len` bytes, retrying short writes. Returns 0 / -1. */
int serial_write_all(int fd, const uint8_t *buf, size_t len);

/* Read up to `len` bytes, blocking at most `timeout_ms` for the first byte
 * (timeout_ms < 0 blocks forever). Returns bytes read (0 == timeout) or -1. */
int serial_read_timeout(int fd, uint8_t *buf, size_t len, int timeout_ms);

/* Discard buffered input and output. */
void serial_flush(int fd);

/* True if `baud` is a rate this layer can set. */
int serial_baud_supported(int baud);

#endif /* UHF_SERIAL_H */
