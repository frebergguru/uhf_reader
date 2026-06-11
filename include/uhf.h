/* uhf.h - R2000 "0xA0" UHF reader protocol (PiSwords UCM6xx / Impinj R2000).
 *
 * This is the protocol spoken by the PiSwords reader that enumerates behind a
 * 1a86:fe0c QinHeng USB-serial bridge and is driven by the vendor's Java app
 * (com.xyang.rfidreader). The reader module reports itself as "UCM601 v2.3".
 * The command set, frame layout and checksum below are taken verbatim from the
 * vendor app's CommandContent / Commands / rfidClient classes. See
 * docs/PROTOCOL.md.
 *
 * Frame layout (same in both directions):
 *
 *     A0  Len  Adr  Cmd  Data...  Checksum
 *
 *   A0       = constant header byte
 *   Len      = Adr + Cmd + Data + Checksum byte count = data_len + 3
 *              (so the whole frame on the wire is Len + 2 bytes)
 *   Adr      = reader address (this unit ships at 0x00; NOT a wildcard scheme)
 *   Cmd      = command byte, echoed in the reply
 *   Checksum = two's complement of the 8-bit sum of every preceding byte:
 *              (~(A0+Len+Adr+Cmd+Data...) + 1) & 0xFF
 *
 * Link defaults for this unit: 9600 8N1, RTS asserted, no flow control, addr 0.
 */
#ifndef UHF_H
#define UHF_H

#include <stddef.h>
#include <stdint.h>

#define UHF_HEAD        0xA0
#define UHF_MAX_FRAME   256
#define UHF_MAX_DATA    250
#define UHF_MAX_EPC     62      /* EPC bank max (bytes) */
#define UHF_ADDR_DEFAULT 0x00   /* this unit's address */

/* Commands (host -> reader), from the vendor app's Commands class. */
enum {
    UHF_CMD_RESET            = 0x70,
    UHF_CMD_SET_BAUD         = 0x71,
    UHF_CMD_GET_FW_VERSION   = 0x72,
    UHF_CMD_SET_ADDRESS      = 0x73,
    UHF_CMD_SET_ANTENNA      = 0x74,
    UHF_CMD_GET_ANTENNA      = 0x75,
    UHF_CMD_SET_POWER        = 0x76,
    UHF_CMD_GET_POWER        = 0x77,
    UHF_CMD_SET_REGION       = 0x78,
    UHF_CMD_GET_REGION       = 0x79,
    UHF_CMD_SET_BEEP         = 0x7A,
    UHF_CMD_GET_TEMPERATURE  = 0x7B,
    UHF_CMD_SET_TEMP_POWER   = 0x66, /* temporary output power             */
    UHF_CMD_SET_4ANT_POWER   = 0x5F,
    UHF_CMD_SET_8ANT_POWER   = 0x5E,
    UHF_CMD_SET_RFLINK       = 0x69, /* RF link profile / bandwidth        */
    UHF_CMD_GET_RFLINK       = 0x6A,
    UHF_CMD_SET_CW           = 0x3E, /* continuous wave                    */
    UHF_CMD_GET_CW           = 0x3F,
    UHF_CMD_SET_TX_TIME      = 0x5D, /* TX on/off duty time                */
    UHF_CMD_GET_TX_TIME      = 0x5C,
    UHF_CMD_SET_SESSION      = 0x5B, /* Session + Target                   */
    UHF_CMD_GET_SESSION      = 0x5A,
    UHF_CMD_CHECK_ANT        = 0xE0, /* antenna-connected check            */
    UHF_CMD_PARA_SAVE        = 0x4A, /* persist current settings to flash  */
    UHF_CMD_PARA_RESET       = 0x4B, /* restore factory defaults           */
    UHF_CMD_SET_KEEPALIVE    = 0x4E,
    UHF_CMD_GET_KEEPALIVE    = 0x4F,
    UHF_CMD_GET_MULTAG       = 0x50, /* multi-tag mode status              */
    UHF_CMD_SET_MULTAG       = 0x51,
    UHF_CMD_GET_GPIO         = 0x60,
    UHF_CMD_SET_GPIO         = 0x61,
    UHF_CMD_INVENTORY        = 0x80, /* buffered inventory          */
    UHF_CMD_READ             = 0x81, /* read tag memory             */
    UHF_CMD_WRITE            = 0x82, /* write tag memory            */
    UHF_CMD_LOCK             = 0x83,
    UHF_CMD_KILL             = 0x84,
    UHF_CMD_SET_EPC_MATCH    = 0x85, /* access EPC match            */
    UHF_CMD_GET_EPC_MATCH    = 0x86,
    UHF_CMD_REALTIME_INV     = 0x89, /* real-time inventory         */
    UHF_CMD_CUSTOM_INV       = 0x8A,
    UHF_CMD_SESSION_TARGET_INV = 0x8B, /* customized session/target inv  */
    UHF_CMD_STOP_INV         = 0x8C,
    UHF_CMD_SET_SELECT       = 0x8D, /* G2 Select command           */
    UHF_CMD_GET_SELECT       = 0x8E,
    UHF_CMD_GET_INV_BUFFER   = 0x90,
    UHF_CMD_GET_RST_BUFFER   = 0x91,
    UHF_CMD_GET_BUF_TAGCOUNT = 0x92,
    UHF_CMD_RESET_BUFFER     = 0x93
};

/* G2 memory banks (read/write). */
enum {
    UHF_MEM_RESERVED = 0x00, /* kill + access passwords */
    UHF_MEM_EPC      = 0x01, /* CRC + PC + EPC          */
    UHF_MEM_TID      = 0x02, /* tag id                  */
    UHF_MEM_USER     = 0x03  /* user memory             */
};

/* Reader return / error codes (ErrorCode class). */
enum {
    UHF_RC_SUCCESS          = 0x10,
    UHF_RC_FAIL             = 0x11,
    UHF_RC_CUSTOM_INV_DONE  = 0x12,
    UHF_RC_FASTANT_INV_DONE = 0x13,
    UHF_RC_TAG_INVENTORY_ERR= 0x31,
    UHF_RC_TAG_READ_ERR     = 0x32,
    UHF_RC_TAG_WRITE_ERR    = 0x33,
    UHF_RC_TAG_LOCK_ERR     = 0x34,
    UHF_RC_TAG_KILL_ERR     = 0x35,
    UHF_RC_MCU_RESET_ERR    = 0x20,
    UHF_RC_CW_ON_ERR        = 0x21,
    UHF_RC_ANT_MISSING_ERR  = 0x22,
    UHF_RC_WRITE_FLASH_ERR  = 0x23,
    UHF_RC_READ_FLASH_ERR   = 0x24,
    UHF_RC_SET_POWER_ERR    = 0x25,
    UHF_RC_NO_TAG           = 0x36,
    UHF_RC_INV_OK_ACC_FAIL  = 0x37,
    UHF_RC_BUFFER_EMPTY     = 0x38,
    UHF_RC_ACCESS_PW_ERR    = 0x40,
    UHF_RC_UNSUPPORTED      = 0x41,
    UHF_RC_WORDCNT_TOO_LONG = 0x43,
    UHF_RC_MEMBANK_RANGE    = 0x44,
    UHF_RC_ADDRESS_INVALID  = 0x46,
    UHF_RC_ANT_ID_RANGE     = 0x47,
    UHF_RC_POWER_RANGE      = 0x48,
    UHF_RC_FREQ_RANGE       = 0x49,
    UHF_RC_BAUD_RANGE       = 0x4A
};

/* A parsed response frame: A0 Len Adr Cmd <data...> Checksum. */
typedef struct {
    uint8_t  adr;
    uint8_t  cmd;
    uint16_t len;                 /* number of bytes in `data`           */
    uint8_t  data[UHF_MAX_FRAME]; /* payload between Cmd and Checksum     */
    int      sum_ok;              /* checksum verified                   */
} uhf_frame_t;

/* One tag from a real-time inventory (0x89) reply. */
typedef struct {
    uint8_t  ant;                 /* antenna id                          */
    uint8_t  pc[2];               /* protocol control word               */
    uint8_t  epc[UHF_MAX_EPC];
    uint8_t  epc_len;
    int32_t  rssi_raw;            /* raw 32-bit RSSI field (uncalibrated) */
    int16_t  rssi_dbm;            /* calibrated RSSI in dBm (-90..0)      */
    uint32_t freq;                /* kHz, when present                   */
    uint8_t  read_count;          /* buffered inventory: times read      */
    int      has_rssi;
    int      has_freq;
} uhf_tag_t;

/* Decoded firmware-version (0x72) reply: "UCM60<model> v<major>.<minor>". */
typedef struct {
    uint8_t major;
    uint8_t minor;
    uint8_t model;
    int     valid;
} uhf_version_t;

/* ---- Checksum ---- */
uint8_t uhf_checksum(const uint8_t *bytes, size_t len);

/* Convert the raw 4-byte inventory RSSI field into calibrated dBm (-90..0),
 * using the vendor's per-hardware lookup tables. `epc_len` is the EPC byte
 * count from the same tag report. */
int uhf_rssi_dbm(int32_t rssi_raw, int epc_len);

/* ---- Frame building ----
 * Build a command frame into `out` (>= UHF_MAX_FRAME). Returns wire length,
 * or -1 if dlen is too large. */
int uhf_build(uint8_t *out, uint8_t adr, uint8_t cmd,
              const uint8_t *data, uint8_t dlen);

int uhf_build_get_version(uint8_t *out, uint8_t adr);
int uhf_build_get_power(uint8_t *out, uint8_t adr);
int uhf_build_set_power(uint8_t *out, uint8_t adr, uint8_t dbm);
int uhf_build_get_antenna(uint8_t *out, uint8_t adr);
int uhf_build_set_antenna(uint8_t *out, uint8_t adr, uint8_t ant_id);
int uhf_build_set_beep(uint8_t *out, uint8_t adr, uint8_t mode); /* 0=off 1=on */
/* Custom (user-defined) frequency region: region byte 0x04, channel spacing,
 * channel count, and start frequency in kHz (24-bit). */
int uhf_build_set_region_custom(uint8_t *out, uint8_t adr, uint16_t spacing,
                                uint8_t count, uint32_t start_khz);
int uhf_build_para_save(uint8_t *out, uint8_t adr);  /* persist to flash      */
int uhf_build_para_reset(uint8_t *out, uint8_t adr); /* factory defaults      */
int uhf_build_realtime_inventory(uint8_t *out, uint8_t adr, uint8_t repeat);
int uhf_build_inventory(uint8_t *out, uint8_t adr, uint8_t repeat);
int uhf_build_stop_inventory(uint8_t *out, uint8_t adr);
int uhf_build_reset(uint8_t *out, uint8_t adr);
int uhf_build_set_address(uint8_t *out, uint8_t adr, uint8_t new_adr);

/* Read tag memory. wordAddr/wordCnt are in 16-bit words; password is 32-bit. */
int uhf_build_read(uint8_t *out, uint8_t adr, uint8_t membank,
                   uint32_t word_addr, uint16_t word_cnt, uint32_t password);
/* Write tag memory. `data` is word_cnt*2 bytes. */
int uhf_build_write(uint8_t *out, uint8_t adr, uint32_t password,
                    uint8_t membank, uint32_t word_addr, uint16_t word_cnt,
                    const uint8_t *data);

/* ---- Streaming parser ---- */
typedef struct {
    uint8_t buf[UHF_MAX_FRAME];
    size_t  len;
    size_t  need;   /* total expected frame length once known (0 = unknown) */
} uhf_parser_t;

void uhf_parser_init(uhf_parser_t *p);

/* Feed one byte; returns 1 and fills *out when a frame completes. */
int uhf_parser_feed(uhf_parser_t *p, uint8_t byte, uhf_frame_t *out);

/* ---- Decoders ---- */
/* Decode a firmware-version (0x72) reply. 0 on success, -1 otherwise. */
int uhf_decode_version(const uhf_frame_t *f, uhf_version_t *v);

/* Decode one tag from a real-time inventory (0x89) reply. Returns 1 if the
 * frame carried a tag, 0 if it was a status/end frame (see *status), -1 on
 * malformed. */
int uhf_decode_realtime_tag(const uhf_frame_t *f, uhf_tag_t *tag, uint8_t *status);

/* Decode one tag from a buffered-inventory buffer reply (0x90 / 0x91). The
 * frame layout is: invDataLen(1) PC(2) EPC(n) RSSI(4) Freq(3) ant(1) count(1).
 * Returns 1 if a tag was decoded, 0 if the frame was an empty/short reply,
 * -1 on malformed. */
int uhf_decode_buffer_tag(const uhf_frame_t *f, uhf_tag_t *tag);

/* ---- Helpers ---- */
void uhf_hex(char *dst, size_t dstsz, const uint8_t *buf, size_t len);
const char *uhf_cmd_name(uint8_t cmd);
const char *uhf_status_name(uint8_t code); /* reader return / error code */

#endif /* UHF_H */
