/* uhf_scan - continuous UHF tag scanner (R2000 0xA0 / PiSwords UCM6xx).
 *
 * Repeatedly issues a real-time inventory and maintains a live table of unique
 * EPCs with read count and age. Ctrl-C stops cleanly.
 */
#include "serial.h"
#include "uhf.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <signal.h>
#include <time.h>
#include <errno.h>

#define MAX_TAGS 512

typedef struct {
    uint8_t  epc[UHF_MAX_EPC];
    uint8_t  epc_len;
    uint32_t count;
    int32_t  rssi;
    double   last_seen;
} tag_entry_t;

static volatile sig_atomic_t g_stop = 0;
static void on_sigint(int sig) { (void)sig; g_stop = 1; }

static double now_sec(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (double)ts.tv_sec + (double)ts.tv_nsec / 1e9;
}

static tag_entry_t g_tags[MAX_TAGS];
static int g_ntags = 0;

static void record_tag(const uhf_tag_t *t)
{
    for (int i = 0; i < g_ntags; i++) {
        if (g_tags[i].epc_len == t->epc_len &&
            memcmp(g_tags[i].epc, t->epc, t->epc_len) == 0) {
            g_tags[i].count++;
            g_tags[i].rssi = t->rssi_dbm;
            g_tags[i].last_seen = now_sec();
            return;
        }
    }
    if (g_ntags >= MAX_TAGS)
        return;
    tag_entry_t *e = &g_tags[g_ntags++];
    memcpy(e->epc, t->epc, t->epc_len);
    e->epc_len = t->epc_len;
    e->count = 1;
    e->rssi = t->rssi_dbm;
    e->last_seen = now_sec();
}

static void redraw(double reads)
{
    char hex[3 * UHF_MAX_EPC];
    printf("\033[H\033[2J");
    printf("uhf_scan  -  %d unique tag(s), %.0f reads   (Ctrl-C to stop)\n",
           g_ntags, reads);
    printf("%-3s %-34s %7s %6s %6s\n", "#", "EPC", "RSSI", "COUNT", "AGE");
    printf("----------------------------------------------------------------\n");
    double now = now_sec();
    for (int i = 0; i < g_ntags; i++) {
        uhf_hex(hex, sizeof hex, g_tags[i].epc, g_tags[i].epc_len);
        printf("%-3d %-34s %4ddBm %6u %5.1fs\n", i + 1, hex,
               g_tags[i].rssi, g_tags[i].count, now - g_tags[i].last_seen);
    }
    fflush(stdout);
}

/* Read frames until the link stays quiet for quiet_ms or cap_s elapses.
 * Returns bytes drained. */
static long swallow(int fd, int quiet_ms, double cap_s)
{
    double hard = now_sec() + cap_s;
    long total = 0;
    for (;;) {
        uint8_t c[256];
        int n = serial_read_timeout(fd, c, sizeof c, quiet_ms);
        if (n > 0) total += n;
        if (n <= 0 || now_sec() >= hard) return total;
    }
}

/* Force the reader out of any free-running inventory: hammer Stop (0x8C) until
 * the stream goes quiet. One Stop is not reliable on this firmware. */
static void calm_reader(int fd, uint8_t adr)
{
    uint8_t frame[UHF_MAX_FRAME];
    int flen = uhf_build_stop_inventory(frame, adr);
    for (int i = 0; i < 8; i++) {
        serial_flush(fd);
        serial_write_all(fd, frame, (size_t)flen);
        if (swallow(fd, 400, 1.5) < 10) break;
    }
}

static void usage(const char *p)
{
    fprintf(stderr,
        "Usage: %s [-d device] [-b baud] [-a addr]\n"
        "  -d device   serial device (default /dev/ttyACM0)\n"
        "  -b baud     baud rate (default 9600)\n"
        "  -a addr     reader address in hex (default 00)\n", p);
}

int main(int argc, char **argv)
{
    const char *path = "/dev/ttyACM0";
    int baud = 9600;
    uint8_t adr = UHF_ADDR_DEFAULT;

    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "-d") && i + 1 < argc) path = argv[++i];
        else if (!strcmp(argv[i], "-b") && i + 1 < argc) baud = atoi(argv[++i]);
        else if (!strcmp(argv[i], "-a") && i + 1 < argc) adr = (uint8_t)strtol(argv[++i], NULL, 16);
        else { usage(argv[0]); return 2; }
    }

    signal(SIGINT, on_sigint);

    int fd = serial_open(path, baud);
    if (fd < 0) {
        fprintf(stderr, "open %s @ %d: %s\n", path, baud, strerror(errno));
        return 1;
    }
    serial_flush(fd);
    swallow(fd, 200, 1.0);     /* clear stray bytes from a prior run */
    calm_reader(fd, adr);      /* stop any inventory left free-running */

    /* Real-time inventory free-runs (streams until Stop), so kick it ONCE and
     * read the stream live — never re-fire it on top of a running stream. */
    uint8_t frame[UHF_MAX_FRAME];
    int flen = uhf_build_realtime_inventory(frame, adr, 1);
    serial_flush(fd);
    serial_write_all(fd, frame, (size_t)flen);

    uhf_parser_t parser;
    uhf_parser_init(&parser);
    double reads = 0, last_draw = 0, last_rx = now_sec();
    int io_err = 0;

    while (!g_stop) {
        uint8_t chunk[256];
        int n = serial_read_timeout(fd, chunk, sizeof chunk, 200);
        if (n < 0) { io_err = 1; break; }
        if (n > 0) {
            last_rx = now_sec();
            for (int i = 0; i < n; i++) {
                uhf_frame_t f;
                if (!uhf_parser_feed(&parser, chunk[i], &f) || !f.sum_ok)
                    continue;
                if (f.cmd != UHF_CMD_REALTIME_INV)
                    continue;
                uhf_tag_t tag;
                uint8_t st;
                if (uhf_decode_realtime_tag(&f, &tag, &st) == 1) {
                    record_tag(&tag);
                    reads++;
                }
            }
        }
        double t = now_sec();
        if (t - last_draw > 0.25) { redraw(reads); last_draw = t; }
        /* If the stream stalls (firmware finished a bounded round), re-kick —
         * but only after real silence, never on top of an active stream. */
        if (t - last_rx > 1.5) {
            serial_flush(fd);
            serial_write_all(fd, frame, (size_t)flen);
            last_rx = t;
        }
    }

    /* halt the free-running inventory and drain to a quiet state on the way out */
    calm_reader(fd, adr);
    redraw(reads);
    if (io_err)
        fprintf(stderr, "\nI/O error: %s\n", strerror(errno));
    printf("\nStopped. %d unique tag(s) seen.\n", g_ntags);
    serial_close(fd);
    return io_err ? 1 : 0;
}
