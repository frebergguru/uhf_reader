# R2000 "0xA0" reader protocol

The device is a PiSwords UHF reader/writer whose RF module reports itself as
**UCM601 v2.3** (the vendor app also supports UCM602 / UCM606(L) / UCM608-H /
UCM608-4 / UCM608-8 — these are module variants, mostly differing in antenna
count). Behind the `1a86:fe0c` QinHeng USB-serial bridge it speaks the
**Impinj R2000 family "0xA0" protocol**.

This file documents what `src/uhf.c` implements. The authoritative source is the
vendor's own (decompiled) Java reader application; the frame format, command
bytes and return codes below were extracted from its `CommandContent`,
`Commands`, `rfidClient` and `ErrorCode` classes.

## Link settings

- **Baud:** this unit ships at **9600** 8N1, no flow control. (Configurable on
  the reader via cmd `0x71`; the vendor combo offers 1200–460800.)
- **RTS:** the host **must assert RTS** (the vendor app calls `setRTS()`); the
  reader stays completely silent otherwise. `serial.c` sets `TIOCM_RTS` on open.
  No DTR/RTS *handshake* or init sequence beyond that is needed.
- **Address:** 1 byte. **This unit answers at address `0x00`.** Unlike the older
  UHFReader family, `0xFF` is **not** a wildcard here — the address must actually
  match, so probing the wrong address just returns silence.
- **USB mode:** enumerates as CDC-ACM (`/dev/ttyACM*`).

> **Two behavioural gotchas, learned the hard way:**
>
> 1. **Inventory free-runs.** Both real-time (`0x89`) and buffered (`0x80`)
>    inventory make the reader stream continuously with no end-of-round marker
>    until it receives **StopInventory (`0x8C`)**. At 9600 baud this saturates
>    the link, so always send Stop (and drain to silence) when done. One Stop is
>    not always enough — hammer it until the stream actually dies.
> 2. **Desync wedges it.** If stray/partial frames arrive (wrong baud) or a
>    command collides with a still-running inventory stream, the reader's framing
>    desyncs and it emits garbage / stale frames; heavy abuse can leave it stuck
>    until a USB power-cycle. `uhf_probe` stops at the first valid reply (never
>    writes at other bauds); the tools always drain to a quiet state after
>    inventory and ignore replies whose command byte doesn't match the request.

## Frame format

Identical in both directions:

```
A0  Len  Adr  Cmd  Data...  Checksum
```

- `A0`       — constant header byte.
- `Len`      — `Adr + Cmd + Data + Checksum` byte count = `len(Data) + 3`.
               The whole frame on the wire is therefore `Len + 2` bytes.
- `Adr`      — reader address (`0x00` on this unit).
- `Cmd`      — command byte, echoed in the reply.
- `Checksum` — two's complement of the 8-bit sum of every preceding byte:
               `(~(A0 + Len + Adr + Cmd + Data...) + 1) & 0xFF`.

### Worked example (verified against the live device)

```
GetFirmwareVersion -> addr 0:   TX  A0 03 00 72 EB
                                RX  A0 06 00 72 02 03 01 E2
```

The reply data `02 03 01` decodes as `major=2, minor=3, model=1` → "UCM601 v2.3"
(checksum `E2`: sum of `A0 06 00 72 02 03 01` = `0x11E`, two's complement of the
low byte `0x1E` = `0xE2`).

## Commands

The command bytes and return codes are enumerated in `include/uhf.h`
(`UHF_CMD_*`, `UHF_RC_*`); the console exposes the whole set (type `help`). The
ones that matter most:

| Cmd  | Name                  | Notes                                       |
|------|-----------------------|---------------------------------------------|
| 0x70 | Reset                 | reset reader                                |
| 0x72 | GetFirmwareVersion    | data `major minor model`                    |
| 0x73 | SetReaderAddress      | data `newAddr`                              |
| 0x74 | SetWorkAntenna        | data `antId`                                |
| 0x75 | GetWorkAntenna        | reply data `antId`                          |
| 0x76 | SetOutputPower        | data `dBm`                                  |
| 0x77 | GetOutputPower        | reply data `dBm` (per antenna)              |
| 0x78 | Set/GetFreqRegion     | `0x79` to read                              |
| 0x7A | SetBeepMode           | data `0`=off `1`=on                         |
| 0x7B | GetReaderTemperature  | reply data `sign temp`                      |
| 0x4A | ParaSave              | persist current settings to flash           |
| 0x4B | ParaReset             | restore factory defaults                    |
| 0x80 | Inventory (buffered)  | data `antId`; accumulates unique tags; **free-runs** |
| 0x81 | Read tag memory       | see below                                   |
| 0x82 | Write tag memory      | see below                                   |
| 0x83 | Lock / 0x84 Kill      | data starts with the 4-byte password        |
| 0x89 | RealTimeInventory     | data `antId`; tags streamed live; **free-runs** |
| 0x8C | StopInventory         | halts a free-running inventory              |
| 0x90 | GetInventoryBuffer    | dump buffered tags (one frame per tag)      |
| 0x91 | GetAndResetInvBuffer  | dump buffered tags **and** clear            |
| 0x92 | GetInvBufferTagCount  | reply data = tag count (2 bytes)            |
| 0x93 | ResetInventoryBuffer  | clear the buffer                            |

Access reads/writes (`0x81`/`0x82`) only target a tag in **single-tag mode**
(set `0x51` data `0x00`); in multi-tag mode they return `0x36` no-tag.

### Frequency region (`0x78` set / `0x79` get)

Standard regions: data = `region(1) startChannel(1) endChannel(1)`.

| code | region | channels (this unit) |
|------|--------|----------------------|
| 1    | FCC    | 7–59  |
| 2    | ETSI   | 0–6   |
| 3    | CHN-1  | 43–53 |
| 4    | custom | user-defined: `04 freqSpace(2) freqQuantity(1) startFreq(3 = kHz)` |
| 5    | CHN-2  | 43–53 |

Examples (verified against the vendor app): `A0 06 00 78 02 00 06 DA` = set ETSI;
`A0 06 00 78 01 07 3B 9F` = set FCC; the `0x79` get reply echoes the same
`region start end` triple.

### RF link profile (`0x69` set / `0x6A` get)

One byte selects the air-interface profile:

| code | profile |
|------|---------|
| 0xD1 | FM0 200KHz tari:6.25us |
| 0xD6 | Miller_4 200KHz tari:25us |
| 0xD7 | Miller_4 250KHz tari:25us |
| 0xD9 | FM0 40KHz tari:25us |
| 0xDA | GB FM0 64KHz tari:6.25us |
| 0xDB | GB FM0 128KHz tari:6.25us |
| 0xDC | GB FM0 64KHz tari:12.5us |
| 0xDD | GB Miller 128KHz tari:12.5us |

### Session / Target (`0x5B` set / `0x5A` get)

data = `session(1) target(1)`. session `0..3` = S0..S3; target `0` = A, `1` = B.

### Select (`0x8D` set / `0x8E` get)

The Gen2 Select filter:

```
enable(1)  selParam(1)  pointer(4)  maskLenBits(1)  truncation(1)  mask(maskLenBits/8)
```

`enable` 0/1; `selParam` packs `Target(bits 7-5) | Action(bits 4-2) | MemBank
(bits 1-0)` where MemBank `0`=RFU `1`=EPC `2`=TID `3`=User; `pointer` is the
bit offset; `truncation` 0/1. Verified get reply (Select disabled, all-FF
12-byte mask at EPC pointer 2): `A0 17 00 8E 00 00 00000002 60 00 FF…FF 65`.

### Read / Write tag memory

`Read` (0x81) data: `memBank(1) wordAddr(4, big-endian) wordCnt(2) password(4)`.
`Write` (0x82) data: `password(4) memBank(1) wordAddr(4) wordCnt(2) data(wordCnt*2)`.

Memory banks: `0x00` reserved (kill+access passwords), `0x01` EPC (CRC+PC+EPC),
`0x02` TID, `0x03` user.

## Real-time inventory reply (`0x89`)

A command-ack or end-of-round frame carries a single status byte (`Len = 4`,
e.g. `0x10` success / `0x36` no-tag). A frame reporting a tag carries:

```
ant(1)  PC(2)  EPC(epcLen)  RSSI(4)  Freq(3)
```

where `epcLen = (PC[0] >> 3) * 2` bytes (the EPC word count lives in the top 5
bits of the PC word). `RSSI` is a 4-byte field whose top byte encodes a rssiMode
(bits 7-5) and hardwareMode (bits 4-1) selecting per-hardware calibration
constants; `uhf_rssi_dbm()` converts it to real dBm
(`B*log10(magnitude/epcLen) + C`, clamped to -90..0), exposed as
`tag.rssi_dbm`. `Freq` is the channel frequency in kHz.

## Buffered inventory reply (`0x90` / `0x91`)

`Inventory` (`0x80`) accumulates *unique* tags into the reader's buffer; reading
the buffer returns **one frame per tag**, each with this payload (note the extra
CRC vs. the real-time format, and that the RSSI block starts at offset
`invDataLen + 1`):

```
invDataLen(1)  PC(2)  EPC(epcLen)  CRC(2)  RSSI(4)  Freq(3)  ant(1)  readCount(1)
```

`invDataLen = 2 + epcLen + 2` (covers PC+EPC+CRC). `readCount` is how many times
that tag was seen. An empty buffer replies with a single status byte (`Len = 4`,
e.g. `0x38` buffer-empty). Verified frame (one tag, 12-byte EPC):

```
A0 1D 00 91 | 10 | 3000 | E20000200914016323809025 | 5304 | E60708CE | 0D3414 | 01 | 43 | E4
            invLen  PC     EPC                          CRC    RSSI       Freq=865300 ant cnt cksum
```

## Read reply (`0x81`)

On success the reply carries the located tag plus the requested words:

```
tagCount(2)  dataLen(1)  PC(2)  EPC(epcLen)  CRC(2)  data(dataLen-4-epcLen)  readLen(2)...
```

On failure it is a single status byte (`0x36` no-tag, `0x40` access/password
error, `0x32` read error, etc.). A single read does one inventory+access round
and often misses, so the console retries until the tag is captured.

## The clone / copy operation

The device is sold as a "copier/cloner". With this protocol that is:

1. **Read source** — real-time inventory (`0x89`); take the source tag's EPC.
2. **Write target** — place a writable tag, then `write` (0x82) the new EPC into
   the EPC bank (bank `0x01`, word address `0x02`, with the access password,
   default all-zero). In the console: `write 1 2 <epc-hex>`.

> Writing/locking tags is destructive. Verify the EPC before writing; a locked
> tag rejects the write (return code `0x40` access/password error).
