/* serial.c - POSIX termios serial layer. */
#define _DEFAULT_SOURCE
#include "serial.h"

#include <termios.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include <string.h>
#include <sys/select.h>
#include <sys/ioctl.h>

static speed_t baud_const(int baud)
{
    switch (baud) {
    case 9600:   return B9600;
    case 19200:  return B19200;
    case 38400:  return B38400;
    case 57600:  return B57600;
    case 115200: return B115200;
    case 230400: return B230400;
    case 460800: return B460800;
    case 921600: return B921600;
    default:     return B0;
    }
}

int serial_baud_supported(int baud)
{
    return baud_const(baud) != B0;
}

int serial_set_baud(int fd, int baud)
{
    speed_t sp = baud_const(baud);
    if (sp == B0) {
        errno = EINVAL;
        return -1;
    }

    struct termios tio;
    if (tcgetattr(fd, &tio) != 0)
        return -1;

    cfmakeraw(&tio);                 /* 8N1, no flow control, no echo */
    tio.c_cflag |= (CLOCAL | CREAD); /* ignore modem lines, enable receiver */
    tio.c_cflag &= ~CRTSCTS;         /* no hardware flow control */
    tio.c_cflag &= ~CSTOPB;          /* one stop bit */
    tio.c_cflag &= ~PARENB;          /* no parity */
    tio.c_cc[VMIN] = 0;              /* timing handled via select() */
    tio.c_cc[VTIME] = 0;

    if (cfsetispeed(&tio, sp) != 0 || cfsetospeed(&tio, sp) != 0)
        return -1;
    if (tcsetattr(fd, TCSANOW, &tio) != 0)
        return -1;

    /* This reader requires RTS asserted (the vendor app calls setRTS()).
     * With hardware flow control off, RTS is a plain output line whose state
     * is otherwise undefined, so set it high explicitly (TIOCMBIS = set bit). */
    int rts = TIOCM_RTS;
    ioctl(fd, TIOCMBIS, &rts);

    tcflush(fd, TCIOFLUSH);
    return 0;
}

int serial_open(const char *path, int baud)
{
    /* Open non-blocking first so we don't hang on modem-control lines. */
    int fd = open(path, O_RDWR | O_NOCTTY | O_NONBLOCK);
    if (fd < 0)
        return -1;

    int fl = fcntl(fd, F_GETFL);
    if (fl < 0 || fcntl(fd, F_SETFL, fl & ~O_NONBLOCK) < 0) {
        int e = errno;
        close(fd);
        errno = e;
        return -1;
    }

    if (serial_set_baud(fd, baud) != 0) {
        int e = errno;
        close(fd);
        errno = e;
        return -1;
    }
    return fd;
}

void serial_close(int fd)
{
    if (fd >= 0)
        close(fd);
}

int serial_write_all(int fd, const uint8_t *buf, size_t len)
{
    size_t off = 0;
    while (off < len) {
        ssize_t n = write(fd, buf + off, len - off);
        if (n < 0) {
            if (errno == EINTR)
                continue;
            return -1;
        }
        off += (size_t)n;
    }
    return 0;
}

int serial_read_timeout(int fd, uint8_t *buf, size_t len, int timeout_ms)
{
    fd_set rf;
    FD_ZERO(&rf);
    FD_SET(fd, &rf);

    struct timeval tv;
    struct timeval *ptv = NULL;
    if (timeout_ms >= 0) {
        tv.tv_sec = timeout_ms / 1000;
        tv.tv_usec = (timeout_ms % 1000) * 1000;
        ptv = &tv;
    }

    int r = select(fd + 1, &rf, NULL, NULL, ptv);
    if (r < 0)
        return (errno == EINTR) ? 0 : -1;
    if (r == 0)
        return 0; /* timeout */

    ssize_t n = read(fd, buf, len);
    if (n < 0)
        return (errno == EINTR || errno == EAGAIN) ? 0 : -1;
    return (int)n;
}

void serial_flush(int fd)
{
    tcflush(fd, TCIOFLUSH);
}
