/* uhf_console - interactive REPL for the R2000 0xA0 reader (PiSwords UCM6xx).
 *
 * Exposes the full command set reverse-engineered from the vendor app's
 * rfidClient.Msg* builders. Commands that have a dedicated library builder use
 * it; the rest build their payload inline and hand it to uhf_build().
 */
#include "serial.h"
#include "uhf.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <time.h>
#include <errno.h>

static uint8_t g_adr = UHF_ADDR_DEFAULT;
static uint8_t g_last_cmd = 0;   /* command byte of the last frame sent */

static double now_sec(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (double)ts.tv_sec + (double)ts.tv_nsec / 1e9;
}

/* Parse hex bytes ("AA BB" or "aabb") into out. Returns count or -1. */
static int parse_hex(const char *s, uint8_t *out, size_t cap)
{
    size_t n = 0;
    int hi = -1;
    for (; *s; s++) {
        if (isspace((unsigned char)*s)) continue;
        int v;
        if (*s >= '0' && *s <= '9') v = *s - '0';
        else if (*s >= 'a' && *s <= 'f') v = *s - 'a' + 10;
        else if (*s >= 'A' && *s <= 'F') v = *s - 'A' + 10;
        else return -1;
        if (hi < 0) hi = v;
        else { if (n >= cap) return -1; out[n++] = (uint8_t)((hi << 4) | v); hi = -1; }
    }
    return (hi >= 0) ? -1 : (int)n;
}

/* Parse an 8-hex-digit access/kill password into a uint32. Returns 0/-1. */
static int parse_pw(const char *s, uint32_t *out)
{
    uint8_t b[4];
    if (!s || parse_hex(s, b, 4) != 4) return -1;
    *out = ((uint32_t)b[0] << 24) | ((uint32_t)b[1] << 16) |
           ((uint32_t)b[2] << 8) | b[3];
    return 0;
}

static int next_int(const char *tok, int dflt)
{ return tok ? (int)strtol(tok, NULL, 0) : dflt; }

/* Read and pretty-print reply frames for up to ms (extends on activity). */
static void drain(int fd, int ms)
{
    uhf_parser_t parser;
    uhf_parser_init(&parser);
    char hex[3 * UHF_MAX_FRAME];
    double end = now_sec() + ms / 1000.0;
    int saw = 0;

    while (now_sec() < end) {
        uint8_t chunk[256];
        int n = serial_read_timeout(fd, chunk, sizeof chunk, 100);
        if (n < 0) { printf("  read error: %s\n", strerror(errno)); return; }
        for (int i = 0; i < n; i++) {
            uhf_frame_t f;
            if (!uhf_parser_feed(&parser, chunk[i], &f))
                continue;
            if (!f.sum_ok) continue;                  /* ignore corrupt bytes  */
            if (g_last_cmd && f.cmd != g_last_cmd)     /* stale reply to a      */
                continue;                             /* previous command      */
            saw = 1;

            if (f.cmd == UHF_CMD_GET_FW_VERSION) {
                uhf_version_t v;
                if (uhf_decode_version(&f, &v) == 0) {
                    if (v.model == 10)
                        printf("  reader: UCM606L v%u.%u\n", v.major, v.minor);
                    else
                        printf("  reader: UCM60%u v%u.%u\n", v.model, v.major, v.minor);
                } else {
                    uhf_hex(hex, sizeof hex, f.data, f.len);
                    printf("  get-version reply: [%s]\n", hex);
                }
            } else if (f.cmd == UHF_CMD_GET_TEMPERATURE && f.len >= 2) {
                printf("  temperature: %c%u C\n",
                       f.data[0] == 0 ? '-' : '+', f.data[1]);
            } else if (f.cmd == UHF_CMD_GET_POWER) {
                printf("  power:");
                for (uint16_t k = 0; k < f.len; k++)
                    printf(" %u dBm", f.data[k]);     /* one byte per antenna */
                printf("\n");
            } else if (f.cmd == UHF_CMD_GET_ANTENNA && f.len >= 1) {
                printf("  work antenna: %u\n", f.data[0]);
            } else if (f.cmd == UHF_CMD_GET_REGION && f.len >= 1) {
                /* region codes (vendor combo FCC/ETSI/CHN-1/CHN-2): 1=FCC
                 * 2=ETSI 3=CHN-1 4=custom 5=CHN-2 */
                static const char *rn[] = { "?", "FCC", "ETSI", "CHN-1",
                                            "custom", "CHN-2" };
                uint8_t reg = f.data[0];
                const char *name = reg <= 5 ? rn[reg] : "?";
                if (reg == 0x04 && f.len >= 7) {
                    unsigned space = ((unsigned)f.data[1] << 8) | f.data[2];
                    unsigned start = ((unsigned)f.data[4] << 16) |
                                     ((unsigned)f.data[5] << 8) | f.data[6];
                    printf("  region: custom  spacing=%u  channels=%u  "
                           "startFreq=%u kHz\n", space, f.data[3], start);
                } else if (f.len >= 3) {
                    printf("  region: %s (0x%02X)  channels %u-%u\n",
                           name, reg, f.data[1], f.data[2]);
                } else {
                    printf("  region: %s (0x%02X)\n", name, reg);
                }
            } else if (f.cmd == UHF_CMD_GET_SESSION && f.len >= 2) {
                printf("  session: S%u  target: %c\n",
                       f.data[0] & 0x03, f.data[1] ? 'B' : 'A');
            } else if (f.cmd == UHF_CMD_GET_RFLINK && f.len >= 1) {
                const char *name;
                switch (f.data[0]) {
                case 0xD1: name = "FM0 200KHz tari:6.25us";      break;
                case 0xD6: name = "Miller_4 200KHz tari:25us";   break;
                case 0xD7: name = "Miller_4 250KHz tari:25us";   break;
                case 0xD9: name = "FM0 40KHz tari:25us";         break;
                case 0xDA: name = "GB FM0 64KHz tari:6.25us";    break;
                case 0xDB: name = "GB FM0 128KHz tari:6.25us";   break;
                case 0xDC: name = "GB FM0 64KHz tari:12.5us";    break;
                case 0xDD: name = "GB Miller 128KHz tari:12.5us"; break;
                default:   name = "?";                           break;
                }
                printf("  rf-link profile: 0x%02X (%s)\n", f.data[0], name);
            } else if (f.cmd == UHF_CMD_GET_SELECT && f.len >= 8) {
                static const char *bank[] = { "RFU", "EPC", "TID", "User" };
                uint8_t en = f.data[0], sp = f.data[1], trunc = f.data[7];
                uint32_t ptr = ((uint32_t)f.data[2] << 24) |
                               ((uint32_t)f.data[3] << 16) |
                               ((uint32_t)f.data[4] << 8) | f.data[5];
                int mask_len = f.data[6] / 8;
                size_t avail = f.len - 8;
                if ((size_t)mask_len > avail) mask_len = (int)avail;
                uhf_hex(hex, sizeof hex, f.data + 8, (size_t)mask_len);
                printf("  select: %s  target S%u  action %u  membank %s  "
                       "ptr 0x%08X  trunc %s\n",
                       en ? "enabled" : "disabled",
                       (sp >> 5) & 0x07, (sp >> 2) & 0x07, bank[sp & 0x03],
                       ptr, trunc ? "on" : "off");
                printf("  mask (%d bytes): %s\n", mask_len,
                       mask_len ? hex : "(none)");
            } else if (f.cmd == UHF_CMD_REALTIME_INV) {
                uhf_tag_t tag;
                uint8_t st;
                int r = uhf_decode_realtime_tag(&f, &tag, &st);
                if (r == 1) {
                    uhf_hex(hex, sizeof hex, tag.epc, tag.epc_len);
                    printf("  TAG  ant=%u  pc=%02X%02X  EPC %s",
                           tag.ant, tag.pc[0], tag.pc[1], hex);
                    if (tag.has_rssi) printf("  rssi=%d dBm", tag.rssi_dbm);
                    if (tag.has_freq) printf("  freq=%u", tag.freq);
                    printf("\n");
                } else if (r == 0) {
                    printf("  (%s)\n", uhf_status_name(st));
                }
            } else if (f.cmd == UHF_CMD_READ) {
                /* Read reply: tagCount(2) dataLen(1) PC(2) EPC(n) CRC(2)
                 * data(dataLen-4-epcLen) readLen(2) ...  (vendor rfid6CRead) */
                if (f.len <= 2) {
                    printf("  read: %s\n", uhf_status_name(f.len ? f.data[0] : 0));
                } else {
                    size_t idx = 2;                 /* skip tagCount */
                    int dataLen = f.data[idx++];
                    uint8_t pc0 = f.data[idx], pc1 = f.data[idx + 1];
                    int epcLen = (pc0 >> 3) * 2;
                    int rdlen = dataLen - 2 - epcLen - 2;
                    size_t epc_off = idx + 2;
                    size_t rd_off = epc_off + (size_t)epcLen + 2;
                    if (epcLen < 0 || rdlen < 0 || rd_off + (size_t)rdlen > f.len) {
                        uhf_hex(hex, sizeof hex, f.data, f.len);
                        printf("  read reply (raw): %s\n", hex);
                    } else {
                        uhf_hex(hex, sizeof hex, f.data + epc_off, (size_t)epcLen);
                        printf("  read OK  pc=%02X%02X  EPC %s\n", pc0, pc1, hex);
                        uhf_hex(hex, sizeof hex, f.data + rd_off, (size_t)rdlen);
                        printf("  data (%d word%s): %s\n",
                               rdlen / 2, rdlen == 2 ? "" : "s",
                               rdlen ? hex : "(none)");
                    }
                }
            } else if (f.len == 0) {
                printf("  %s: (ack)\n", uhf_cmd_name(f.cmd));
            } else if (f.len == 1) {
                /* One byte: a recognised return code, else a raw value. */
                const char *nm = uhf_status_name(f.data[0]);
                if (strcmp(nm, "code?") != 0)
                    printf("  %s: %s\n", uhf_cmd_name(f.cmd), nm);
                else
                    printf("  %s: 0x%02X (%u)\n", uhf_cmd_name(f.cmd),
                           f.data[0], f.data[0]);
            } else {
                uhf_hex(hex, sizeof hex, f.data, f.len);
                printf("  %s: %s\n", uhf_cmd_name(f.cmd), hex);
            }
            end = now_sec() + ms / 1000.0;
        }
    }
    if (!saw)
        printf("  (no reply)\n");
}

static int send_frame(int fd, const uint8_t *frame, int len)
{
    char hex[3 * UHF_MAX_FRAME];
    if (len < 0) { printf("  build failed (bad args)\n"); return -1; }
    uhf_hex(hex, sizeof hex, frame, (size_t)len);
    printf("  >> %s\n", hex);
    if (len >= 4) g_last_cmd = frame[3];   /* expected reply command byte */
    serial_flush(fd);
    return serial_write_all(fd, frame, (size_t)len);
}

/* Read frames until the link stays quiet for `quiet_ms` or `cap_s` elapses.
 * Returns the number of bytes drained. */
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
static void calm_reader(int fd)
{
    uint8_t frame[UHF_MAX_FRAME];
    int flen = uhf_build_stop_inventory(frame, g_adr);
    for (int i = 0; i < 8; i++) {
        serial_flush(fd);
        serial_write_all(fd, frame, (size_t)flen);
        if (swallow(fd, 400, 1.5) < 10) break;   /* quiet -> done */
    }
}

/* Real-time inventory for a bounded window, then Stop + full drain.
 *
 * This firmware's inventory (both 0x80 and 0x89) FREE-RUNS: one command makes
 * the reader stream tag frames continuously with no end-of-round marker. So we
 * read for a hard time cap, send Stop (0x8C), then drain until the link is
 * quiet — that last drain is essential: if any streamed bytes are left in the
 * pipe the next command collides with them and desyncs the reader (→ replug).
 * Unique EPCs are printed once each (no per-read flood). */
static void run_inventory(int fd, double seconds)
{
    uint8_t frame[UHF_MAX_FRAME];
    char hex[3 * UHF_MAX_FRAME];

    int flen = uhf_build_realtime_inventory(frame, g_adr, 1);
    serial_flush(fd);
    serial_write_all(fd, frame, (size_t)flen);

    uhf_parser_t parser;
    uhf_parser_init(&parser);
    uint8_t seen[64][UHF_MAX_EPC];
    uint8_t seen_len[64];
    int nseen = 0;
    long reads = 0;
    double end = now_sec() + seconds;
    while (now_sec() < end) {
        uint8_t chunk[256];
        int n = serial_read_timeout(fd, chunk, sizeof chunk, 100);
        if (n < 0) break;
        for (int i = 0; i < n; i++) {
            uhf_frame_t f;
            if (!uhf_parser_feed(&parser, chunk[i], &f) || !f.sum_ok) continue;
            if (f.cmd != UHF_CMD_REALTIME_INV) continue;
            uhf_tag_t tag; uint8_t st;
            if (uhf_decode_realtime_tag(&f, &tag, &st) != 1) continue;
            reads++;
            int dup = 0;
            for (int k = 0; k < nseen; k++)
                if (seen_len[k] == tag.epc_len &&
                    memcmp(seen[k], tag.epc, tag.epc_len) == 0) { dup = 1; break; }
            if (!dup && nseen < 64) {
                memcpy(seen[nseen], tag.epc, tag.epc_len);
                seen_len[nseen++] = tag.epc_len;
                uhf_hex(hex, sizeof hex, tag.epc, tag.epc_len);
                printf("  TAG  ant=%u  pc=%02X%02X  rssi=%d dBm  EPC %s\n",
                       tag.ant, tag.pc[0], tag.pc[1], tag.rssi_dbm, hex);
            }
        }
    }

    /* halt the free-running inventory — hammer Stop until the stream dies */
    calm_reader(fd);

    if (nseen == 0) printf("  (no tag)\n");
    else printf("  %d unique tag(s), %ld read(s)\n", nseen, reads);
}

/* Bounded BUFFERED inventory (binv): clear buffer -> start inventory (0x80) and
 * let the reader accumulate unique tags in its buffer for `seconds` -> Stop ->
 * read+clear the buffer (0x91). The buffer dedups in the reader and reports a
 * per-tag read count. Like real-time inventory, 0x80 free-runs, so it must be
 * stopped; everything else here is bounded. */
static void run_buffered_inventory(int fd, double seconds)
{
    uint8_t frame[UHF_MAX_FRAME];
    char hex[3 * UHF_MAX_FRAME];
    int flen;

    /* 1. clear any stale buffered tags */
    flen = uhf_build(frame, g_adr, UHF_CMD_RESET_BUFFER, NULL, 0);
    serial_flush(fd); serial_write_all(fd, frame, (size_t)flen);
    swallow(fd, 200, 0.5);

    /* 2. start buffered inventory; discard the streamed 0x80 count replies */
    flen = uhf_build_inventory(frame, g_adr, 1);   /* antenna 1 */
    serial_flush(fd); serial_write_all(fd, frame, (size_t)flen);
    double end = now_sec() + seconds;
    while (now_sec() < end) {
        uint8_t c[256];
        if (serial_read_timeout(fd, c, sizeof c, 100) < 0) break;
    }

    /* 3. halt the free-running inventory */
    calm_reader(fd);

    /* 4. read + clear the accumulated buffer: one 0x91 frame per unique tag */
    flen = uhf_build(frame, g_adr, UHF_CMD_GET_RST_BUFFER, NULL, 0);
    serial_flush(fd); serial_write_all(fd, frame, (size_t)flen);

    uhf_parser_t p; uhf_parser_init(&p);
    double quiet = now_sec() + 0.7;
    int seen = 0;
    while (now_sec() < quiet) {
        uint8_t chunk[256];
        int n = serial_read_timeout(fd, chunk, sizeof chunk, 100);
        if (n < 0) break;
        for (int i = 0; i < n; i++) {
            uhf_frame_t f;
            if (!uhf_parser_feed(&p, chunk[i], &f) || !f.sum_ok) continue;
            if (f.cmd != UHF_CMD_GET_RST_BUFFER) continue;  /* ignore stop-acks etc. */
            quiet = now_sec() + 0.4;                         /* extend while draining */
            uhf_tag_t tag;
            if (uhf_decode_buffer_tag(&f, &tag) == 1) {
                uhf_hex(hex, sizeof hex, tag.epc, tag.epc_len);
                printf("  TAG  ant=%u  pc=%02X%02X  reads=%u  rssi=%d dBm  EPC %s",
                       tag.ant, tag.pc[0], tag.pc[1], tag.read_count,
                       tag.rssi_dbm, hex);
                if (tag.has_freq) printf("  freq=%u", tag.freq);
                printf("\n");
                seen++;
            }
        }
    }
    if (!seen) printf("  (no tags buffered)\n");
    else printf("  %d unique tag(s) in buffer\n", seen);
}

/* Pretty-print the data payload of a successful read (0x81) reply. */
static void print_read_payload(const uhf_frame_t *f)
{
    char hex[3 * UHF_MAX_FRAME];
    size_t idx = 2;                     /* skip tagCount */
    int dataLen = f->data[idx++];
    uint8_t pc0 = f->data[idx], pc1 = f->data[idx + 1];
    int epcLen = (pc0 >> 3) * 2;
    int rdlen = dataLen - 2 - epcLen - 2;
    size_t epc_off = idx + 2;
    size_t rd_off = epc_off + (size_t)epcLen + 2;
    if (epcLen < 0 || rdlen < 0 || rd_off + (size_t)rdlen > f->len) {
        uhf_hex(hex, sizeof hex, f->data, f->len);
        printf("  read reply (raw): %s\n", hex);
        return;
    }
    uhf_hex(hex, sizeof hex, f->data + epc_off, (size_t)epcLen);
    printf("  read OK  pc=%02X%02X  EPC %s\n", pc0, pc1, hex);
    uhf_hex(hex, sizeof hex, f->data + rd_off, (size_t)rdlen);
    printf("  data (%d word%s): %s\n", rdlen / 2, rdlen == 2 ? "" : "s",
           rdlen ? hex : "(none)");
}

/* Issue a read, retrying while the tag isn't captured (status 0x36/0x11/0x37).
 * A single read does one inventory+access round and often misses, so retry. */
static void issue_read(int fd, uint8_t adr, uint8_t bank, uint32_t waddr,
                       uint16_t wcnt, uint32_t pw)
{
    uint8_t frame[UHF_MAX_FRAME];
    int flen = uhf_build_read(frame, adr, bank, waddr, wcnt, pw);
    const int ATTEMPTS = 10;
    for (int attempt = 0; attempt < ATTEMPTS; attempt++) {
        serial_flush(fd);
        serial_write_all(fd, frame, (size_t)flen);
        uhf_parser_t parser;
        uhf_parser_init(&parser);
        double end = now_sec() + 1.0;
        while (now_sec() < end) {
            uint8_t chunk[256];
            int n = serial_read_timeout(fd, chunk, sizeof chunk, 100);
            if (n < 0) { printf("  read error: %s\n", strerror(errno)); return; }
            int retry = 0;
            for (int i = 0; i < n; i++) {
                uhf_frame_t f;
                if (!uhf_parser_feed(&parser, chunk[i], &f)) continue;
                if (f.cmd != UHF_CMD_READ || !f.sum_ok) continue;
                if (f.len <= 2) {
                    uint8_t st = f.len ? f.data[0] : 0;
                    if (st == UHF_RC_NO_TAG || st == UHF_RC_FAIL ||
                        st == UHF_RC_INV_OK_ACC_FAIL) { retry = 1; break; }
                    printf("  read: %s\n", uhf_status_name(st)); /* hard error */
                    return;
                }
                print_read_payload(&f);
                return;
            }
            if (retry) break;           /* re-send and try again */
        }
    }
    printf("  read: no tag captured after %d tries (reposition the tag?)\n",
           ATTEMPTS);
}

static void help(void)
{
    printf(
    "System:\n"
    "  info                 firmware version (0x72)\n"
    "  temp                 reader temperature (0x7B)\n"
    "  reset                reboot the reader (0x70)\n"
    "  save                 persist settings to flash (0x4A)\n"
    "  factory              restore factory defaults (0x4B)\n"
    "  checkant             check antenna connection (0xE0)\n"
    "  addr <hex>           LOCAL: set address used for outgoing commands\n"
    "  setaddr <hex>        reader: change its address (0x73)\n"
    "  baud <code>          reader: set UART baud code (0x71)\n"
    "RF / power:\n"
    "  power [dbm]          get/set output power (0x77/0x76)\n"
    "  temppower <dbm>      set temporary power (0x66)\n"
    "  power4 <a b c d>     set 4-antenna power (0x5F)\n"
    "  power8 <a..h>        set 8-antenna power (0x5E)\n"
    "  antenna [id]         get/set work antenna (0x75/0x74)\n"
    "  region [r st en]     get/set freq region (0x79/0x78); r: 1=FCC 2=ETSI\n"
    "                       3=CHN-1 5=CHN-2; st/en = channel range\n"
    "  region custom <spacingKHz> <channels> <startFreqKHz>   user-defined band\n"
    "  rflink [profile]     get/set RF link profile (0x6A/0x69)\n"
    "  cw [on|off]          get/set continuous wave (0x3F/0x3E)\n"
    "  txtime [on off]      get/set TX on/off time, ms (0x5C/0x5D)\n"
    "  session [s t]        get/set Session and Target (0x5A/0x5B)\n"
    "  beep <on|off>        enable/disable buzzer (0x7A)\n"
    "Inventory:\n"
    "  one                  single real-time inventory (0x89)\n"
    "  scan [secs]          real-time inventory loop (default 3s)\n"
    "  binv [secs]          buffered inventory: clear, accumulate, read buffer\n"
    "  inventory [repeat]   raw buffered inventory command (0x80)\n"
    "  stop                 stop inventory (0x8C)\n"
    "  custom <ant> <num> <mmode> <mnum>   custom inventory (0x8A)\n"
    "  sessioninv <s> <t> <ant>            session-target inventory (0x8B)\n"
    "  multag [status]      get/set multi-tag mode (0x50/0x51)\n"
    "  buffer               read inventory buffer (0x90)\n"
    "  bufferreset          read+clear inventory buffer (0x91)\n"
    "  buffercount          buffer tag count (0x92)\n"
    "  clearbuffer          reset inventory buffer (0x93)\n"
    "Tag access (G2):\n"
    "  dump [pw8]                          read all 4 banks of the tag\n"
    "  read <bank> <wAddr> <wCnt> [pw8]    read tag memory (0x81)\n"
    "  write <bank> <wAddr> <hex> [pw8]    write tag memory (0x82)\n"
    "  lock <pw8> <bank> <locktype>        lock tag region (0x83)\n"
    "  kill <pw8>                          kill tag (0x84)\n"
    "  match <mode> [epchex]               set access EPC match (0x85)\n"
    "  getmatch                            get access EPC match (0x86)\n"
    "  select <en> <param> <ptr> <mlen> <trunc> <maskhex>  set select (0x8D)\n"
    "  getselect                           get select params (0x8E)\n"
    "GPIO / keepalive:\n"
    "  gpio                 read GPIO status (0x60)\n"
    "  setgpio <name> <st>  set GPIO (0x61)\n"
    "  keepalive [en time]  get/set heartbeat (0x4F/0x4E)\n"
    "Raw:\n"
    "  cmd <cmdhex> [data]  build+send a framed command (auto Len/checksum)\n"
    "  raw <hex...>         send exact bytes verbatim\n"
    "  help                 this list\n"
    "  quit                 exit\n");
    printf("  banks: 0=reserved 1=epc 2=tid 3=user   bool args accept on/off/1/0\n");
}

static void usage(const char *p)
{
    fprintf(stderr, "Usage: %s [-d device] [-b baud] [-a addr]\n", p);
}

/* Parse a boolean token (on/1/true vs anything else). */
static uint8_t parse_bool(const char *s)
{
    return (s && (!strcmp(s, "on") || !strcmp(s, "1") || !strcmp(s, "true"))) ? 1 : 0;
}

int main(int argc, char **argv)
{
    const char *path = "/dev/ttyACM0";
    int baud = 9600;

    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "-d") && i + 1 < argc) path = argv[++i];
        else if (!strcmp(argv[i], "-b") && i + 1 < argc) baud = atoi(argv[++i]);
        else if (!strcmp(argv[i], "-a") && i + 1 < argc) g_adr = (uint8_t)strtol(argv[++i], NULL, 16);
        else { usage(argv[0]); return 2; }
    }

    int fd = serial_open(path, baud);
    if (fd < 0) {
        fprintf(stderr, "open %s @ %d: %s\n", path, baud, strerror(errno));
        return 1;
    }
    serial_flush(fd);
    calm_reader(fd);   /* stop any inventory a prior session left free-running */
    printf("uhf_console on %s @ %d baud, addr 0x%02X. Type 'help'.\n",
           path, baud, g_adr);

    char line[512];
    uint8_t frame[UHF_MAX_FRAME];
    uint8_t d[UHF_MAX_DATA];
    int len;

    while (printf("uhf> "), fflush(stdout), fgets(line, sizeof line, stdin)) {
        line[strcspn(line, "\r\n")] = '\0';
        char *cmd = strtok(line, " \t");
        if (!cmd) continue;

        /* ---- System ---- */
        if (!strcmp(cmd, "quit") || !strcmp(cmd, "exit")) {
            break;
        } else if (!strcmp(cmd, "help")) {
            help();
        } else if (!strcmp(cmd, "info")) {
            len = uhf_build_get_version(frame, g_adr);
            send_frame(fd, frame, len); drain(fd, 500);
        } else if (!strcmp(cmd, "temp")) {
            len = uhf_build(frame, g_adr, UHF_CMD_GET_TEMPERATURE, NULL, 0);
            send_frame(fd, frame, len); drain(fd, 500);
        } else if (!strcmp(cmd, "reset")) {
            len = uhf_build_reset(frame, g_adr);
            send_frame(fd, frame, len); drain(fd, 800);
        } else if (!strcmp(cmd, "save")) {
            len = uhf_build_para_save(frame, g_adr);
            send_frame(fd, frame, len); drain(fd, 800);
        } else if (!strcmp(cmd, "factory")) {
            len = uhf_build_para_reset(frame, g_adr);
            send_frame(fd, frame, len); drain(fd, 800);
        } else if (!strcmp(cmd, "checkant")) {
            len = uhf_build(frame, g_adr, UHF_CMD_CHECK_ANT, NULL, 0);
            send_frame(fd, frame, len); drain(fd, 600);
        } else if (!strcmp(cmd, "addr")) {
            char *a = strtok(NULL, " \t");
            if (a) g_adr = (uint8_t)strtol(a, NULL, 16);
            printf("  addr = 0x%02X\n", g_adr);
        } else if (!strcmp(cmd, "setaddr")) {
            char *a = strtok(NULL, " \t");
            if (!a) { printf("  setaddr: need new address hex\n"); continue; }
            len = uhf_build_set_address(frame, g_adr, (uint8_t)strtol(a, NULL, 16));
            send_frame(fd, frame, len); drain(fd, 500);
        } else if (!strcmp(cmd, "baud")) {
            char *a = strtok(NULL, " \t");
            if (!a) { printf("  baud: need a baud-code byte\n"); continue; }
            d[0] = (uint8_t)strtol(a, NULL, 0);
            len = uhf_build(frame, g_adr, UHF_CMD_SET_BAUD, d, 1);
            send_frame(fd, frame, len); drain(fd, 500);

        /* ---- RF / power ---- */
        } else if (!strcmp(cmd, "power")) {
            char *a = strtok(NULL, " \t");
            if (a) len = uhf_build_set_power(frame, g_adr, (uint8_t)atoi(a));
            else   len = uhf_build_get_power(frame, g_adr);
            send_frame(fd, frame, len); drain(fd, 500);
        } else if (!strcmp(cmd, "temppower")) {
            char *a = strtok(NULL, " \t");
            if (!a) { printf("  temppower: need dBm\n"); continue; }
            d[0] = (uint8_t)atoi(a);
            len = uhf_build(frame, g_adr, UHF_CMD_SET_TEMP_POWER, d, 1);
            send_frame(fd, frame, len); drain(fd, 500);
        } else if (!strcmp(cmd, "power4")) {
            int ok = 1;
            for (int k = 0; k < 4; k++) { char *t = strtok(NULL, " \t"); if (!t) ok = 0; else d[k] = (uint8_t)atoi(t); }
            if (!ok) { printf("  power4: need 4 dBm values\n"); continue; }
            len = uhf_build(frame, g_adr, UHF_CMD_SET_4ANT_POWER, d, 4);
            send_frame(fd, frame, len); drain(fd, 500);
        } else if (!strcmp(cmd, "power8")) {
            int ok = 1;
            for (int k = 0; k < 8; k++) { char *t = strtok(NULL, " \t"); if (!t) ok = 0; else d[k] = (uint8_t)atoi(t); }
            if (!ok) { printf("  power8: need 8 dBm values\n"); continue; }
            len = uhf_build(frame, g_adr, UHF_CMD_SET_8ANT_POWER, d, 8);
            send_frame(fd, frame, len); drain(fd, 500);
        } else if (!strcmp(cmd, "antenna")) {
            char *a = strtok(NULL, " \t");
            if (a) len = uhf_build_set_antenna(frame, g_adr, (uint8_t)atoi(a));
            else   len = uhf_build_get_antenna(frame, g_adr);
            send_frame(fd, frame, len); drain(fd, 500);
        } else if (!strcmp(cmd, "region")) {
            char *r = strtok(NULL, " \t");
            if (!r) {
                len = uhf_build(frame, g_adr, UHF_CMD_GET_REGION, NULL, 0);
            } else if (!strcmp(r, "custom")) {
                char *sp = strtok(NULL, " \t"), *ct = strtok(NULL, " \t");
                char *sf = strtok(NULL, " \t");
                if (!sp || !ct || !sf) {
                    printf("  region custom: need <spacingKHz> <channels> <startFreqKHz>\n");
                    continue;
                }
                len = uhf_build_set_region_custom(frame, g_adr,
                          (uint16_t)strtol(sp, NULL, 0),
                          (uint8_t)strtol(ct, NULL, 0),
                          (uint32_t)strtol(sf, NULL, 0));
            } else {
                char *st = strtok(NULL, " \t"), *en = strtok(NULL, " \t");
                if (!st || !en) { printf("  region: need <region> <startCh> <endCh>  (or: region custom ...)\n"); continue; }
                d[0] = (uint8_t)strtol(r, NULL, 0);
                d[1] = (uint8_t)strtol(st, NULL, 0);
                d[2] = (uint8_t)strtol(en, NULL, 0);
                len = uhf_build(frame, g_adr, UHF_CMD_SET_REGION, d, 3);
            }
            send_frame(fd, frame, len); drain(fd, 500);
        } else if (!strcmp(cmd, "rflink")) {
            char *a = strtok(NULL, " \t");
            if (a) { d[0] = (uint8_t)strtol(a, NULL, 0); len = uhf_build(frame, g_adr, UHF_CMD_SET_RFLINK, d, 1); }
            else     len = uhf_build(frame, g_adr, UHF_CMD_GET_RFLINK, NULL, 0);
            send_frame(fd, frame, len); drain(fd, 500);
        } else if (!strcmp(cmd, "cw")) {
            char *a = strtok(NULL, " \t");
            if (a) { d[0] = parse_bool(a); len = uhf_build(frame, g_adr, UHF_CMD_SET_CW, d, 1); }
            else     len = uhf_build(frame, g_adr, UHF_CMD_GET_CW, NULL, 0);
            send_frame(fd, frame, len); drain(fd, 500);
        } else if (!strcmp(cmd, "txtime")) {
            char *on = strtok(NULL, " \t"), *off = strtok(NULL, " \t");
            if (!on) { len = uhf_build(frame, g_adr, UHF_CMD_GET_TX_TIME, NULL, 0); }
            else if (!off) { printf("  txtime: need <onTime> <offTime>\n"); continue; }
            else {
                int t_on = next_int(on, 0), t_off = next_int(off, 0);
                d[0] = (uint8_t)(t_on >> 8); d[1] = (uint8_t)t_on;
                d[2] = (uint8_t)(t_off >> 8); d[3] = (uint8_t)t_off;
                len = uhf_build(frame, g_adr, UHF_CMD_SET_TX_TIME, d, 4);
            }
            send_frame(fd, frame, len); drain(fd, 500);
        } else if (!strcmp(cmd, "session")) {
            char *s = strtok(NULL, " \t"), *t = strtok(NULL, " \t");
            if (!s) { len = uhf_build(frame, g_adr, UHF_CMD_GET_SESSION, NULL, 0); }
            else if (!t) { printf("  session: need <session> <target>\n"); continue; }
            else { d[0] = (uint8_t)next_int(s, 0); d[1] = (uint8_t)next_int(t, 0);
                   len = uhf_build(frame, g_adr, UHF_CMD_SET_SESSION, d, 2); }
            send_frame(fd, frame, len); drain(fd, 500);
        } else if (!strcmp(cmd, "beep")) {
            char *a = strtok(NULL, " \t");
            if (!a) { printf("  beep: need on|off\n"); continue; }
            uint8_t mode = parse_bool(a);
            len = uhf_build_set_beep(frame, g_adr, mode);
            send_frame(fd, frame, len); drain(fd, 500);
            printf("  buzzer %s\n", mode ? "enabled" : "disabled");

        /* ---- Inventory ---- */
        } else if (!strcmp(cmd, "one")) {
            /* Read the one tag's EPC in single-tag mode: bounded and reliable,
             * with none of the free-running-inventory stress. */
            len = uhf_build(frame, g_adr, UHF_CMD_SET_MULTAG, (uint8_t[]){0}, 1);
            send_frame(fd, frame, len); drain(fd, 300);
            issue_read(fd, g_adr, UHF_MEM_EPC, 2, 6, 0);
        } else if (!strcmp(cmd, "scan")) {
            char *a = strtok(NULL, " \t");
            int secs = a ? atoi(a) : 3;
            if (secs <= 0) secs = 3;
            run_inventory(fd, (double)secs);
        } else if (!strcmp(cmd, "binv")) {
            char *a = strtok(NULL, " \t");
            double secs = a ? atof(a) : 2.0;
            if (secs <= 0) secs = 2.0;
            run_buffered_inventory(fd, secs);
        } else if (!strcmp(cmd, "inventory")) {
            char *a = strtok(NULL, " \t");
            len = uhf_build_inventory(frame, g_adr, (uint8_t)next_int(a, 1));
            send_frame(fd, frame, len); drain(fd, 600);
        } else if (!strcmp(cmd, "stop")) {
            len = uhf_build_stop_inventory(frame, g_adr);
            send_frame(fd, frame, len); drain(fd, 400);
        } else if (!strcmp(cmd, "custom")) {
            char *a = strtok(NULL, " \t"), *num = strtok(NULL, " \t");
            char *mm = strtok(NULL, " \t"), *mn = strtok(NULL, " \t");
            if (!a || !num || !mm || !mn) { printf("  custom: need <ant> <invNum> <matchMode> <matchNum>\n"); continue; }
            d[0] = (uint8_t)next_int(a, 0); d[1] = (uint8_t)next_int(num, 0);
            d[2] = (uint8_t)next_int(mm, 0); d[3] = (uint8_t)next_int(mn, 0);
            len = uhf_build(frame, g_adr, UHF_CMD_CUSTOM_INV, d, 4);
            send_frame(fd, frame, len); drain(fd, 600);
        } else if (!strcmp(cmd, "sessioninv")) {
            char *s = strtok(NULL, " \t"), *t = strtok(NULL, " \t"), *an = strtok(NULL, " \t");
            if (!s || !t || !an) { printf("  sessioninv: need <session> <target> <antNum>\n"); continue; }
            d[0] = (uint8_t)next_int(s, 0); d[1] = (uint8_t)next_int(t, 0); d[2] = (uint8_t)next_int(an, 0);
            len = uhf_build(frame, g_adr, UHF_CMD_SESSION_TARGET_INV, d, 3);
            send_frame(fd, frame, len); drain(fd, 600);
        } else if (!strcmp(cmd, "multag")) {
            char *a = strtok(NULL, " \t");
            if (a) { d[0] = (uint8_t)next_int(a, 0); len = uhf_build(frame, g_adr, UHF_CMD_SET_MULTAG, d, 1); }
            else     len = uhf_build(frame, g_adr, UHF_CMD_GET_MULTAG, NULL, 0);
            send_frame(fd, frame, len); drain(fd, 500);
        } else if (!strcmp(cmd, "buffer")) {
            len = uhf_build(frame, g_adr, UHF_CMD_GET_INV_BUFFER, NULL, 0);
            send_frame(fd, frame, len); drain(fd, 800);
        } else if (!strcmp(cmd, "bufferreset")) {
            len = uhf_build(frame, g_adr, UHF_CMD_GET_RST_BUFFER, NULL, 0);
            send_frame(fd, frame, len); drain(fd, 800);
        } else if (!strcmp(cmd, "buffercount")) {
            len = uhf_build(frame, g_adr, UHF_CMD_GET_BUF_TAGCOUNT, NULL, 0);
            send_frame(fd, frame, len); drain(fd, 500);
        } else if (!strcmp(cmd, "clearbuffer")) {
            len = uhf_build(frame, g_adr, UHF_CMD_RESET_BUFFER, NULL, 0);
            send_frame(fd, frame, len); drain(fd, 500);

        /* ---- Tag access ---- */
        } else if (!strcmp(cmd, "dump")) {
            char *pw = strtok(NULL, " \t");
            uint32_t pwv = 0;
            if (pw && parse_pw(pw, &pwv)) { printf("  dump: password = 8 hex digits\n"); continue; }
            /* (bank, startWord, wordCount) for each EPC Gen2 bank. Counts are
             * generous; a tag with a shorter bank answers 0x43 length-error,
             * which is printed per-bank rather than fatal. */
            /* Access-reads only target a tag in single-tag mode; multi-tag
             * mode makes every read return no-tag. Switch it on for the dump. */
            len = uhf_build(frame, g_adr, UHF_CMD_SET_MULTAG, (uint8_t[]){0}, 1);
            send_frame(fd, frame, len); drain(fd, 400);
            const struct { uint8_t bank; uint16_t start, cnt; const char *name; } banks[] = {
                { UHF_MEM_RESERVED, 0, 4, "Reserved (kill+access pw)" },
                { UHF_MEM_EPC,      0, 8, "EPC (CRC+PC+EPC)" },
                { UHF_MEM_TID,      0, 6, "TID" },
                { UHF_MEM_USER,     0, 8, "User" },
            };
            for (size_t k = 0; k < sizeof banks / sizeof banks[0]; k++) {
                printf("--- bank %u: %s ---\n", banks[k].bank, banks[k].name);
                issue_read(fd, g_adr, banks[k].bank, banks[k].start, banks[k].cnt, pwv);
            }
        } else if (!strcmp(cmd, "read")) {
            char *bk = strtok(NULL, " \t"), *wa = strtok(NULL, " \t");
            char *wc = strtok(NULL, " \t"), *pw = strtok(NULL, " \t");
            if (!bk || !wa || !wc) { printf("  read: need <bank> <wordAddr> <wordCnt> [pw8]\n"); continue; }
            uint32_t pwv = 0;
            if (pw && parse_pw(pw, &pwv)) { printf("  read: password = 8 hex digits\n"); continue; }
            issue_read(fd, g_adr, (uint8_t)strtol(bk,NULL,0),
                       (uint32_t)strtol(wa,NULL,0), (uint16_t)strtol(wc,NULL,0), pwv);
        } else if (!strcmp(cmd, "write")) {
            char *bk = strtok(NULL, " \t"), *wa = strtok(NULL, " \t");
            char *dh = strtok(NULL, " \t"), *pw = strtok(NULL, " \t");
            if (!bk || !wa || !dh) { printf("  write: need <bank> <wordAddr> <hexdata> [pw8]\n"); continue; }
            uint8_t wd[UHF_MAX_DATA];
            int wl = parse_hex(dh, wd, sizeof wd);
            if (wl <= 0 || wl % 2) { printf("  write: hexdata must be whole 16-bit words\n"); continue; }
            uint32_t pwv = 0;
            if (pw && parse_pw(pw, &pwv)) { printf("  write: password = 8 hex digits\n"); continue; }
            len = uhf_build_write(frame, g_adr, pwv, (uint8_t)strtol(bk,NULL,0),
                                  (uint32_t)strtol(wa,NULL,0), (uint16_t)(wl/2), wd);
            printf("  writing %d word(s) to bank %s...\n", wl/2, bk);
            send_frame(fd, frame, len); drain(fd, 1500);
        } else if (!strcmp(cmd, "lock")) {
            char *pw = strtok(NULL, " \t"), *bk = strtok(NULL, " \t"), *lt = strtok(NULL, " \t");
            if (!pw || !bk || !lt) { printf("  lock: need <pw8> <bank> <lockType>\n"); continue; }
            uint8_t pb[4];
            if (parse_hex(pw, pb, 4) != 4) { printf("  lock: password = 8 hex digits\n"); continue; }
            memcpy(d, pb, 4);
            d[4] = (uint8_t)strtol(bk, NULL, 0);
            d[5] = (uint8_t)strtol(lt, NULL, 0);
            len = uhf_build(frame, g_adr, UHF_CMD_LOCK, d, 6);
            send_frame(fd, frame, len); drain(fd, 1200);
        } else if (!strcmp(cmd, "kill")) {
            char *pw = strtok(NULL, " \t");
            uint8_t pb[4];
            if (!pw || parse_hex(pw, pb, 4) != 4) { printf("  kill: need <pw8> (8 hex digits)\n"); continue; }
            len = uhf_build(frame, g_adr, UHF_CMD_KILL, pb, 4);
            send_frame(fd, frame, len); drain(fd, 1200);
        } else if (!strcmp(cmd, "match")) {
            char *mode = strtok(NULL, " \t"), *eh = strtok(NULL, " \t");
            if (!mode) { printf("  match: need <mode> [epchex]\n"); continue; }
            size_t n = 0;
            d[n++] = (uint8_t)strtol(mode, NULL, 0);
            if (eh) {
                uint8_t epc[UHF_MAX_EPC];
                int el = parse_hex(eh, epc, sizeof epc);
                if (el <= 0) { printf("  match: bad EPC hex\n"); continue; }
                d[n++] = (uint8_t)el;
                memcpy(d + n, epc, (size_t)el); n += (size_t)el;
            }
            len = uhf_build(frame, g_adr, UHF_CMD_SET_EPC_MATCH, d, (uint8_t)n);
            send_frame(fd, frame, len); drain(fd, 500);
        } else if (!strcmp(cmd, "getmatch")) {
            len = uhf_build(frame, g_adr, UHF_CMD_GET_EPC_MATCH, NULL, 0);
            send_frame(fd, frame, len); drain(fd, 500);
        } else if (!strcmp(cmd, "select")) {
            char *en = strtok(NULL, " \t"), *pr = strtok(NULL, " \t"), *ptr = strtok(NULL, " \t");
            char *ml = strtok(NULL, " \t"), *tr = strtok(NULL, " \t"), *mh = strtok(NULL, " \t");
            if (!en || !pr || !ptr || !ml || !tr || !mh) {
                printf("  select: need <enable> <selParam> <pointer> <maskLen> <trunc> <maskhex>\n"); continue; }
            uint8_t mask[64];
            int mlb = parse_hex(mh, mask, sizeof mask);
            if (mlb < 0) { printf("  select: bad mask hex\n"); continue; }
            uint32_t pointer = (uint32_t)strtol(ptr, NULL, 0);
            size_t n = 0;
            d[n++] = (uint8_t)strtol(en, NULL, 0);
            d[n++] = (uint8_t)strtol(pr, NULL, 0);
            d[n++] = (uint8_t)(pointer >> 24); d[n++] = (uint8_t)(pointer >> 16);
            d[n++] = (uint8_t)(pointer >> 8);  d[n++] = (uint8_t)pointer;
            d[n++] = (uint8_t)strtol(ml, NULL, 0);
            d[n++] = (uint8_t)strtol(tr, NULL, 0);
            memcpy(d + n, mask, (size_t)mlb); n += (size_t)mlb;
            len = uhf_build(frame, g_adr, UHF_CMD_SET_SELECT, d, (uint8_t)n);
            send_frame(fd, frame, len); drain(fd, 500);
        } else if (!strcmp(cmd, "getselect")) {
            len = uhf_build(frame, g_adr, UHF_CMD_GET_SELECT, NULL, 0);
            send_frame(fd, frame, len); drain(fd, 500);

        /* ---- GPIO / keepalive ---- */
        } else if (!strcmp(cmd, "gpio")) {
            len = uhf_build(frame, g_adr, UHF_CMD_GET_GPIO, NULL, 0);
            send_frame(fd, frame, len); drain(fd, 500);
        } else if (!strcmp(cmd, "setgpio")) {
            char *nm = strtok(NULL, " \t"), *st = strtok(NULL, " \t");
            if (!nm || !st) { printf("  setgpio: need <gpioName> <status>\n"); continue; }
            d[0] = (uint8_t)strtol(nm, NULL, 0); d[1] = (uint8_t)strtol(st, NULL, 0);
            len = uhf_build(frame, g_adr, UHF_CMD_SET_GPIO, d, 2);
            send_frame(fd, frame, len); drain(fd, 500);
        } else if (!strcmp(cmd, "keepalive")) {
            char *en = strtok(NULL, " \t"), *tm = strtok(NULL, " \t");
            if (!en) { len = uhf_build(frame, g_adr, UHF_CMD_GET_KEEPALIVE, NULL, 0); }
            else if (!tm) { printf("  keepalive: need <enable> <reportTime>\n"); continue; }
            else {
                int t = next_int(tm, 0);
                d[0] = (uint8_t)next_int(en, 0);
                d[1] = (uint8_t)(t >> 8); d[2] = (uint8_t)t;
                len = uhf_build(frame, g_adr, UHF_CMD_SET_KEEPALIVE, d, 3);
            }
            send_frame(fd, frame, len); drain(fd, 500);

        /* ---- Raw ---- */
        } else if (!strcmp(cmd, "cmd")) {
            char *ch = strtok(NULL, " \t");
            char *rest = strtok(NULL, "");
            if (!ch) { printf("  cmd: need a command byte\n"); continue; }
            uint8_t cb[1];
            if (parse_hex(ch, cb, 1) != 1) { printf("  cmd: bad command byte\n"); continue; }
            int dl = 0;
            if (rest) { dl = parse_hex(rest, d, sizeof d); if (dl < 0) { printf("  cmd: bad data hex\n"); continue; } }
            len = uhf_build(frame, g_adr, cb[0], d, (uint8_t)dl);
            send_frame(fd, frame, len); drain(fd, 600);
        } else if (!strcmp(cmd, "raw")) {
            char *rest = strtok(NULL, "");
            if (!rest) { printf("  raw: need hex bytes\n"); continue; }
            len = parse_hex(rest, frame, sizeof frame);
            if (len <= 0) { printf("  raw: bad hex\n"); continue; }
            send_frame(fd, frame, len); drain(fd, 600);
        } else {
            printf("  unknown command '%s' (try 'help')\n", cmd);
        }
    }

    serial_close(fd);
    printf("bye\n");
    return 0;
}
