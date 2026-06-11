/* uhf_probe - confirm the reader speaks the R2000 0xA0 protocol and find its
 * baud/address.
 *
 * For each baud it sends GetFirmwareVersion (cmd 0x72) to a set of candidate
 * addresses and reports any valid 0xA0 reply. This unit ships at 9600 baud,
 * address 0 (the protocol has no wildcard address, so the right address must
 * actually match). Use -l <baud> for a long passive capture.
 */
#include "serial.h"
#include "uhf.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>

static const int kBauds[] = { 9600, 115200, 57600, 38400, 19200, 230400 };
static const uint8_t kAddrs[] = { 0x00, 0xFF, 0x01 };

/* Read up to total_ms (extending on activity); append to raw[]. Returns count
 * of valid 0xA0 frames seen. */
static int collect(int fd, int total_ms, uint8_t *raw, size_t *raw_len, size_t cap)
{
    uhf_parser_t parser;
    uhf_parser_init(&parser);
    int valid = 0, budget = total_ms;
    while (budget > 0) {
        uint8_t chunk[128];
        int n = serial_read_timeout(fd, chunk, sizeof chunk, 120);
        if (n < 0) break;
        if (n == 0) { budget -= 120; continue; }
        for (int i = 0; i < n; i++) {
            if (*raw_len < cap) raw[(*raw_len)++] = chunk[i];
            uhf_frame_t f;
            if (uhf_parser_feed(&parser, chunk[i], &f) && f.sum_ok)
                valid++;
        }
        budget = 200;
    }
    return valid;
}

static int listen_mode(const char *path, int baud)
{
    int fd = serial_open(path, baud);
    if (fd < 0) { fprintf(stderr, "open: %s\n", strerror(errno)); return 1; }
    serial_flush(fd);
    printf("Listening on %s @ %d baud (Ctrl-C to stop)...\n", path, baud);
    for (;;) {
        uint8_t chunk[256];
        int n = serial_read_timeout(fd, chunk, sizeof chunk, 1000);
        if (n < 0) break;
        if (n == 0) continue;
        char hex[3 * 256];
        uhf_hex(hex, sizeof hex, chunk, (size_t)n);
        printf("RX %s\n", hex);
        fflush(stdout);
    }
    serial_close(fd);
    return 0;
}

int main(int argc, char **argv)
{
    const char *path = "/dev/ttyACM0";
    int listen = 0, listen_baud = 9600;

    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "-l")) {
            listen = 1;
            if (i + 1 < argc && argv[i + 1][0] != '-') listen_baud = atoi(argv[++i]);
        } else if (argv[i][0] != '-') {
            path = argv[i];
        }
    }
    if (listen) return listen_mode(path, listen_baud);

    printf("uhf_probe: looking for an R2000 (0xA0) reader on %s\n\n", path);

    char hex[3 * UHF_MAX_FRAME];
    int best_baud = 0;
    uint8_t best_addr = 0;

    for (size_t b = 0; b < sizeof kBauds / sizeof kBauds[0]; b++) {
        int baud = kBauds[b];
        int fd = serial_open(path, baud);
        if (fd < 0) { fprintf(stderr, "  %6d: %s\n", baud, strerror(errno)); continue; }
        serial_flush(fd);
        printf("  %6d baud\n", baud);

        uint8_t raw[512];
        int baud_valid = 0;

        for (size_t k = 0; k < sizeof kAddrs / sizeof kAddrs[0]; k++) {
            uint8_t frame[UHF_MAX_FRAME];
            int flen = uhf_build_get_version(frame, kAddrs[k]);
            serial_flush(fd);
            serial_write_all(fd, frame, (size_t)flen);
            size_t raw_len = 0;
            int v = collect(fd, 300, raw, &raw_len, sizeof raw);
            if (raw_len) {
                uhf_hex(hex, sizeof hex, frame, (size_t)flen);
                printf("        get-version @%02X TX %s\n", kAddrs[k], hex);
                uhf_hex(hex, sizeof hex, raw, raw_len);
                printf("                       RX %s%s\n", hex,
                       v ? "   <- valid 0xA0 frame!" : "");
            }
            if (v && !best_baud) { best_baud = baud; best_addr = kAddrs[k]; }
            baud_valid += v;
            if (v) break;   /* stop probing once it answers */
        }
        if (!baud_valid)
            printf("        (silent to all addresses)\n");
        serial_close(fd);
        /* Found it: do NOT keep writing framed bytes at other bauds — at the
         * reader's real baud those look like garbage and can wedge its MCU
         * until a power-cycle. */
        if (best_baud) break;
    }

    printf("\n");
    if (best_baud) {
        printf("R2000 (0xA0) reader detected at %d baud, address 0x%02X.\n",
               best_baud, best_addr);
        printf("Run:  ./uhf_scan -d %s -b %d -a %02X\n", path, best_baud, best_addr);
        return 0;
    }
    printf("No 0xA0 reply at any baud. Confirm the reader is powered and in\n"
           "serial (CDC-ACM) mode, then capture passively while reading a tag:\n"
           "  ./uhf_probe -l 9600 %s\n", path);
    return 1;
}
