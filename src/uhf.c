/* uhf.c - R2000 "0xA0" UHF reader protocol implementation.
 *
 * Frame: A0 Len Adr Cmd Data... Checksum   (Len = data_len + 3)
 * Checksum = two's complement of the 8-bit sum of all preceding bytes.
 * Mirrors the vendor app's CommandContent/rfidClient logic. See docs/PROTOCOL.md.
 */
#include "uhf.h"

#include <stdio.h>
#include <string.h>
#include <math.h>

/* ---- RSSI calibration ----
 * Ported from the vendor app's myRfidCallback.calculateRssi + para_B/para_C.
 * The raw field encodes rssiMode (top 3 bits of the high byte) and
 * hardwareMode (next 4 bits); the low 25 bits are the magnitude. dBm =
 * clamp(B*log10(magnitude/epcLen) + C, -90, 0). */
int uhf_rssi_dbm(int32_t rssi_raw, int epc_len)
{
    static const int para_B[5][8] = {
        { 43, 43, 45, 49, 43, 43, 45, 49 },
        { 43, 43, 45, 49, 43, 43, 45, 49 },
        { 43, 43, 45, 49, 43, 43, 45, 49 },
        { 53, 53, 48, 43, 49, 45, 43, 43 },
        { 47, 47, 47, 47, 46, 43, 43, 43 },
    };
    static const int para_C[5][8] = {
        {  43,  43,  45,  49,  43,  43,  45,  49 },
        {  43,  43,  45,  49,  43,  43,  45,  49 },
        {  43,  43,  45,  49,  43,  43,  45,  49 },
        { -283, -283, -283, -283, -283, -283, -283, -283 },
        { -303, -283, -253, -238, -304, -313, -280, -266 },
    };
    uint32_t raw = (uint32_t)rssi_raw;
    int rssi_mode = (int)(((raw >> 24) & 0xE0) >> 5);   /* 0..7 */
    int hw_mode   = (int)(((raw >> 24) & 0x1E) >> 1);   /* 0..4 valid */
    uint32_t mag  = raw & 0x01FFFFFFu;

    if (epc_len <= 0) epc_len = 1;
    if (rssi_mode > 7 || hw_mode > 4) return -90;

    uint32_t q = mag / (uint32_t)epc_len;               /* integer division */
    if (q == 0) return -90;                             /* log10(0) guard */

    int B = para_B[hw_mode][rssi_mode];
    int C = para_C[hw_mode][rssi_mode];
    int val = (int)((double)B * log10((double)q) + (double)C);
    if (val > 0)  val = 0;
    if (val < -90) val = -90;
    return val;
}

/* ---- Checksum ---- */
uint8_t uhf_checksum(const uint8_t *bytes, size_t len)
{
    uint8_t sum = 0;
    for (size_t i = 0; i < len; i++)
        sum = (uint8_t)(sum + bytes[i]);
    return (uint8_t)(~sum + 1);
}

/* ---- Frame building ---- */
int uhf_build(uint8_t *out, uint8_t adr, uint8_t cmd,
              const uint8_t *data, uint8_t dlen)
{
    if (dlen > UHF_MAX_DATA) return -1;
    size_t i = 0;
    out[i++] = UHF_HEAD;
    out[i++] = (uint8_t)(dlen + 3);   /* Adr + Cmd + Data + Checksum */
    out[i++] = adr;
    out[i++] = cmd;
    if (dlen && data) { memcpy(out + i, data, dlen); i += dlen; }
    out[i] = uhf_checksum(out, i);
    i++;
    return (int)i;
}

int uhf_build_get_version(uint8_t *out, uint8_t adr)
{ return uhf_build(out, adr, UHF_CMD_GET_FW_VERSION, NULL, 0); }

int uhf_build_get_power(uint8_t *out, uint8_t adr)
{ return uhf_build(out, adr, UHF_CMD_GET_POWER, NULL, 0); }

int uhf_build_set_power(uint8_t *out, uint8_t adr, uint8_t dbm)
{ return uhf_build(out, adr, UHF_CMD_SET_POWER, &dbm, 1); }

int uhf_build_get_antenna(uint8_t *out, uint8_t adr)
{ return uhf_build(out, adr, UHF_CMD_GET_ANTENNA, NULL, 0); }

int uhf_build_set_antenna(uint8_t *out, uint8_t adr, uint8_t ant_id)
{ return uhf_build(out, adr, UHF_CMD_SET_ANTENNA, &ant_id, 1); }

int uhf_build_set_beep(uint8_t *out, uint8_t adr, uint8_t mode)
{ return uhf_build(out, adr, UHF_CMD_SET_BEEP, &mode, 1); }

int uhf_build_set_region_custom(uint8_t *out, uint8_t adr, uint16_t spacing,
                                uint8_t count, uint32_t start_khz)
{
    uint8_t d[7];
    d[0] = 0x04;                          /* region = custom/user-defined */
    d[1] = (uint8_t)(spacing >> 8);
    d[2] = (uint8_t)(spacing);
    d[3] = count;
    d[4] = (uint8_t)(start_khz >> 16);
    d[5] = (uint8_t)(start_khz >> 8);
    d[6] = (uint8_t)(start_khz);
    return uhf_build(out, adr, UHF_CMD_SET_REGION, d, sizeof d);
}

int uhf_build_para_save(uint8_t *out, uint8_t adr)
{ return uhf_build(out, adr, UHF_CMD_PARA_SAVE, NULL, 0); }

int uhf_build_para_reset(uint8_t *out, uint8_t adr)
{ return uhf_build(out, adr, UHF_CMD_PARA_RESET, NULL, 0); }

int uhf_build_realtime_inventory(uint8_t *out, uint8_t adr, uint8_t repeat)
{ return uhf_build(out, adr, UHF_CMD_REALTIME_INV, &repeat, 1); }

int uhf_build_inventory(uint8_t *out, uint8_t adr, uint8_t repeat)
{ return uhf_build(out, adr, UHF_CMD_INVENTORY, &repeat, 1); }

int uhf_build_stop_inventory(uint8_t *out, uint8_t adr)
{ return uhf_build(out, adr, UHF_CMD_STOP_INV, NULL, 0); }

int uhf_build_reset(uint8_t *out, uint8_t adr)
{ return uhf_build(out, adr, UHF_CMD_RESET, NULL, 0); }

int uhf_build_set_address(uint8_t *out, uint8_t adr, uint8_t new_adr)
{ return uhf_build(out, adr, UHF_CMD_SET_ADDRESS, &new_adr, 1); }

int uhf_build_read(uint8_t *out, uint8_t adr, uint8_t membank,
                   uint32_t word_addr, uint16_t word_cnt, uint32_t password)
{
    uint8_t d[11];
    d[0]  = membank;
    d[1]  = (uint8_t)(word_addr >> 24);
    d[2]  = (uint8_t)(word_addr >> 16);
    d[3]  = (uint8_t)(word_addr >> 8);
    d[4]  = (uint8_t)(word_addr);
    d[5]  = (uint8_t)(word_cnt >> 8);
    d[6]  = (uint8_t)(word_cnt);
    d[7]  = (uint8_t)(password >> 24);
    d[8]  = (uint8_t)(password >> 16);
    d[9]  = (uint8_t)(password >> 8);
    d[10] = (uint8_t)(password);
    return uhf_build(out, adr, UHF_CMD_READ, d, sizeof d);
}

int uhf_build_write(uint8_t *out, uint8_t adr, uint32_t password,
                    uint8_t membank, uint32_t word_addr, uint16_t word_cnt,
                    const uint8_t *data)
{
    uint8_t d[UHF_MAX_DATA];
    size_t n = 0;
    if ((size_t)word_cnt * 2 + 11 > sizeof d) return -1;
    d[n++] = (uint8_t)(password >> 24);
    d[n++] = (uint8_t)(password >> 16);
    d[n++] = (uint8_t)(password >> 8);
    d[n++] = (uint8_t)(password);
    d[n++] = membank;
    d[n++] = (uint8_t)(word_addr >> 24);
    d[n++] = (uint8_t)(word_addr >> 16);
    d[n++] = (uint8_t)(word_addr >> 8);
    d[n++] = (uint8_t)(word_addr);
    d[n++] = (uint8_t)(word_cnt >> 8);
    d[n++] = (uint8_t)(word_cnt);
    memcpy(d + n, data, (size_t)word_cnt * 2);
    n += (size_t)word_cnt * 2;
    return uhf_build(out, adr, UHF_CMD_WRITE, d, (uint8_t)n);
}

/* ---- Streaming parser ----
 * Frame = A0 Len ... ; total wire length = Len + 2. */
void uhf_parser_init(uhf_parser_t *p)
{
    p->len = 0;
    p->need = 0;
}

int uhf_parser_feed(uhf_parser_t *p, uint8_t byte, uhf_frame_t *out)
{
    /* Resync: the first byte of any frame must be the header. */
    if (p->len == 0) {
        if (byte != UHF_HEAD) return 0;
        p->buf[p->len++] = byte;
        p->need = 0;
        return 0;
    }

    p->buf[p->len++] = byte;

    if (p->len == 2) {
        /* second byte is Len; whole frame is Len + 2 bytes */
        p->need = (size_t)byte + 2;
        if (p->need < 5 || p->need > UHF_MAX_FRAME) {
            /* implausible length: drop and resync */
            p->len = 0;
            p->need = 0;
        }
        return 0;
    }

    if (p->need && p->len >= p->need) {
        size_t total = p->need;
        uint8_t want = uhf_checksum(p->buf, total - 1);
        out->adr = p->buf[2];
        out->cmd = p->buf[3];
        out->len = (uint16_t)(total - 5);   /* minus A0,Len,Adr,Cmd,Checksum */
        memcpy(out->data, p->buf + 4, out->len);
        out->sum_ok = (want == p->buf[total - 1]);
        p->len = 0;
        p->need = 0;
        return 1;
    }

    if (p->len >= UHF_MAX_FRAME) { p->len = 0; p->need = 0; }
    return 0;
}

/* ---- Decoders ---- */
int uhf_decode_version(const uhf_frame_t *f, uhf_version_t *v)
{
    if (f->cmd != UHF_CMD_GET_FW_VERSION || f->len < 3) return -1;
    v->major = f->data[0];
    v->minor = f->data[1];
    v->model = f->data[2];
    v->valid = 1;
    return 0;
}

/* Real-time inventory (0x89) reply.
 *   tag frame : ant(1) PC(2) EPC(epcLen) RSSI(4) Freq(3)
 *               epcLen = (PC[0] >> 3) * 2
 *   status    : a single byte (e.g. 0x10 success / 0x36 no tag) -> returns 0 */
int uhf_decode_realtime_tag(const uhf_frame_t *f, uhf_tag_t *tag, uint8_t *status)
{
    if (f->cmd != UHF_CMD_REALTIME_INV) return -1;

    if (f->len <= 2) {
        if (status) *status = f->len ? f->data[0] : 0;
        return 0;
    }

    memset(tag, 0, sizeof *tag);
    size_t i = 0;
    tag->ant = f->data[i++];
    tag->pc[0] = f->data[i++];
    tag->pc[1] = f->data[i++];

    int epc_len = (tag->pc[0] >> 3) * 2;     /* (PC[0] >> 3) * 2 */
    if (epc_len < 0 || (size_t)epc_len > sizeof tag->epc) return -1;
    if (i + (size_t)epc_len > f->len) return -1;
    memcpy(tag->epc, f->data + i, (size_t)epc_len);
    tag->epc_len = (uint8_t)epc_len;
    i += (size_t)epc_len;

    if (i + 4 <= f->len) {
        tag->rssi_raw = (int32_t)(((uint32_t)f->data[i] << 24) |
                                  ((uint32_t)f->data[i + 1] << 16) |
                                  ((uint32_t)f->data[i + 2] << 8) |
                                  (uint32_t)f->data[i + 3]);
        tag->rssi_dbm = (int16_t)uhf_rssi_dbm(tag->rssi_raw, tag->epc_len);
        tag->has_rssi = 1;
        i += 4;
    }
    if (i + 3 <= f->len) {
        tag->freq = ((uint32_t)f->data[i] << 16) |
                    ((uint32_t)f->data[i + 1] << 8) |
                    (uint32_t)f->data[i + 2];
        tag->has_freq = 1;
        i += 3;
    }
    if (status) *status = UHF_RC_SUCCESS;
    return 1;
}

/* Buffered-inventory buffer reply (0x90 / 0x91), per the vendor parser:
 *   invDataLen(1) PC(2) EPC(epcLen) CRC(2) RSSI(4) Freq(3) ant(1) count(1)
 *   epcLen = (PC[0] >> 3) * 2,  invDataLen = 2 + epcLen + 2 (covers PC+EPC+CRC).
 * The RSSI block starts at offset invDataLen+1 (skipping PC+EPC+CRC). */
int uhf_decode_buffer_tag(const uhf_frame_t *f, uhf_tag_t *tag)
{
    if (f->cmd != UHF_CMD_GET_INV_BUFFER && f->cmd != UHF_CMD_GET_RST_BUFFER)
        return -1;
    if (f->len <= 2)
        return 0;                       /* empty buffer / short reply */

    memset(tag, 0, sizeof *tag);
    uint8_t inv_data_len = f->data[0];
    tag->pc[0] = f->data[1];
    tag->pc[1] = f->data[2];
    int epc_len = (tag->pc[0] >> 3) * 2;
    if (epc_len < 0 || (size_t)epc_len > sizeof tag->epc) return -1;
    if (3 + (size_t)epc_len > f->len) return -1;
    memcpy(tag->epc, f->data + 3, (size_t)epc_len);
    tag->epc_len = (uint8_t)epc_len;

    size_t i = (size_t)inv_data_len + 1;   /* RSSI starts after PC+EPC+CRC */
    if (i + 4 <= f->len) {
        tag->rssi_raw = (int32_t)(((uint32_t)f->data[i] << 24) |
                                  ((uint32_t)f->data[i + 1] << 16) |
                                  ((uint32_t)f->data[i + 2] << 8) |
                                  (uint32_t)f->data[i + 3]);
        tag->rssi_dbm = (int16_t)uhf_rssi_dbm(tag->rssi_raw, tag->epc_len);
        tag->has_rssi = 1;
        i += 4;
    }
    if (i + 3 <= f->len) {
        tag->freq = ((uint32_t)f->data[i] << 16) |
                    ((uint32_t)f->data[i + 1] << 8) | (uint32_t)f->data[i + 2];
        tag->has_freq = 1;
        i += 3;
    }
    if (i < f->len) tag->ant = f->data[i++];
    if (i < f->len) tag->read_count = f->data[i++];
    return 1;
}

/* ---- Helpers ---- */
void uhf_hex(char *dst, size_t dstsz, const uint8_t *buf, size_t len)
{
    size_t o = 0;
    for (size_t i = 0; i < len && o + 3 < dstsz; i++)
        o += (size_t)snprintf(dst + o, dstsz - o, "%02X ", buf[i]);
    if (o && o < dstsz) dst[o - 1] = '\0';   /* drop trailing space */
    else if (dstsz) dst[0] = '\0';
}

const char *uhf_cmd_name(uint8_t cmd)
{
    switch (cmd) {
    case UHF_CMD_RESET:            return "reset";
    case UHF_CMD_SET_BAUD:         return "set-baud";
    case UHF_CMD_GET_FW_VERSION:   return "get-fw-version";
    case UHF_CMD_SET_ADDRESS:      return "set-address";
    case UHF_CMD_SET_ANTENNA:      return "set-antenna";
    case UHF_CMD_GET_ANTENNA:      return "get-antenna";
    case UHF_CMD_SET_POWER:        return "set-power";
    case UHF_CMD_GET_POWER:        return "get-power";
    case UHF_CMD_SET_REGION:       return "set-region";
    case UHF_CMD_GET_REGION:       return "get-region";
    case UHF_CMD_SET_BEEP:         return "set-beep";
    case UHF_CMD_PARA_SAVE:        return "para-save";
    case UHF_CMD_PARA_RESET:       return "para-reset";
    case UHF_CMD_GET_TEMPERATURE:  return "get-temperature";
    case UHF_CMD_SET_TEMP_POWER:   return "set-temp-power";
    case UHF_CMD_SET_4ANT_POWER:   return "set-4ant-power";
    case UHF_CMD_SET_8ANT_POWER:   return "set-8ant-power";
    case UHF_CMD_SET_RFLINK:       return "set-rflink";
    case UHF_CMD_GET_RFLINK:       return "get-rflink";
    case UHF_CMD_SET_CW:           return "set-cw";
    case UHF_CMD_GET_CW:           return "get-cw";
    case UHF_CMD_SET_TX_TIME:      return "set-tx-time";
    case UHF_CMD_GET_TX_TIME:      return "get-tx-time";
    case UHF_CMD_SET_SESSION:      return "set-session";
    case UHF_CMD_GET_SESSION:      return "get-session";
    case UHF_CMD_CHECK_ANT:        return "check-ant";
    case UHF_CMD_SET_KEEPALIVE:    return "set-keepalive";
    case UHF_CMD_GET_KEEPALIVE:    return "get-keepalive";
    case UHF_CMD_GET_MULTAG:       return "get-multag";
    case UHF_CMD_SET_MULTAG:       return "set-multag";
    case UHF_CMD_GET_GPIO:         return "get-gpio";
    case UHF_CMD_SET_GPIO:         return "set-gpio";
    case UHF_CMD_SET_EPC_MATCH:    return "set-epc-match";
    case UHF_CMD_GET_EPC_MATCH:    return "get-epc-match";
    case UHF_CMD_SESSION_TARGET_INV: return "session-target-inv";
    case UHF_CMD_SET_SELECT:       return "set-select";
    case UHF_CMD_GET_SELECT:       return "get-select";
    case UHF_CMD_INVENTORY:        return "inventory";
    case UHF_CMD_READ:             return "read";
    case UHF_CMD_WRITE:            return "write";
    case UHF_CMD_LOCK:             return "lock";
    case UHF_CMD_KILL:             return "kill";
    case UHF_CMD_REALTIME_INV:     return "realtime-inventory";
    case UHF_CMD_CUSTOM_INV:       return "custom-inventory";
    case UHF_CMD_STOP_INV:         return "stop-inventory";
    case UHF_CMD_GET_INV_BUFFER:   return "get-buffer";
    case UHF_CMD_GET_RST_BUFFER:   return "get-reset-buffer";
    case UHF_CMD_GET_BUF_TAGCOUNT: return "buffer-tag-count";
    case UHF_CMD_RESET_BUFFER:     return "reset-buffer";
    default:                       return "cmd?";
    }
}

const char *uhf_status_name(uint8_t code)
{
    switch (code) {
    case UHF_RC_SUCCESS:           return "success";
    case UHF_RC_FAIL:              return "fail";
    case UHF_RC_CUSTOM_INV_DONE:   return "custom-inventory-done";
    case UHF_RC_FASTANT_INV_DONE:  return "fast-ant-inventory-done";
    case UHF_RC_TAG_INVENTORY_ERR: return "tag-inventory-error";
    case UHF_RC_TAG_READ_ERR:      return "tag-read-error";
    case UHF_RC_TAG_WRITE_ERR:     return "tag-write-error";
    case UHF_RC_TAG_LOCK_ERR:      return "tag-lock-error";
    case UHF_RC_TAG_KILL_ERR:      return "tag-kill-error";
    case UHF_RC_MCU_RESET_ERR:     return "mcu-reset-error";
    case UHF_RC_CW_ON_ERR:         return "cw-on-error";
    case UHF_RC_ANT_MISSING_ERR:   return "antenna-missing";
    case UHF_RC_WRITE_FLASH_ERR:   return "write-flash-error";
    case UHF_RC_READ_FLASH_ERR:    return "read-flash-error";
    case UHF_RC_SET_POWER_ERR:     return "set-power-error";
    case UHF_RC_NO_TAG:            return "no-tag";
    case UHF_RC_INV_OK_ACC_FAIL:   return "inventory-ok-access-fail";
    case UHF_RC_BUFFER_EMPTY:      return "buffer-empty";
    case UHF_RC_ACCESS_PW_ERR:     return "access/password-error";
    case UHF_RC_UNSUPPORTED:       return "unsupported";
    case UHF_RC_WORDCNT_TOO_LONG:  return "word-count-too-long";
    case UHF_RC_MEMBANK_RANGE:     return "membank-out-of-range";
    case UHF_RC_ADDRESS_INVALID:   return "address-invalid";
    case UHF_RC_ANT_ID_RANGE:      return "antenna-id-out-of-range";
    case UHF_RC_POWER_RANGE:       return "power-out-of-range";
    case UHF_RC_FREQ_RANGE:        return "freq-region-out-of-range";
    case UHF_RC_BAUD_RANGE:        return "baudrate-out-of-range";
    default:                       return "code?";
    }
}
