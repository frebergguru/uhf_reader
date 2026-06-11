# uhf_reader

A C toolkit for the **PiSwords UHF reader/writer** whose RF module reports as
**UCM601** (EPC C1G2 reader/writer/copier). It connects through a `1a86:fe0c`
QinHeng USB-serial bridge (`/dev/ttyACM*`) and speaks the **Impinj R2000
"0xA0" protocol** (reverse-engineered from the vendor's Java app — see
[`docs/PROTOCOL.md`](docs/PROTOCOL.md)).

```
libuhf.a              serial layer + R2000 0xA0 frame protocol
├─ uhf_probe          confirm the reader + find its baud/address
├─ uhf_scan           continuous real-time inventory scanner
└─ uhf_console        interactive REPL (info / power / scan / read / write / raw)
```

## Build

```sh
make            # -> uhf_probe, uhf_scan, uhf_console, libuhf.a
```

C11 + POSIX only, no external dependencies.

## Device permissions

`/dev/ttyACM*` is owned by `root:uucp` (mode 0660) and your user is not in the
`uucp` group. Pick one:

```sh
sg uucp -c './uhf_probe'                 # one-off, current command only
sudo usermod -aG uucp $USER              # permanent (re-login afterwards)
# or a udev rule granting your login session access:
echo 'SUBSYSTEM=="tty", ATTRS{idVendor}=="1a86", ATTRS{idProduct}=="fe0c", TAG+="uaccess"' \
  | sudo tee /etc/udev/rules.d/60-uhf-reader.rules
sudo udevadm control --reload && sudo udevadm trigger
```

The node number can change across replugs; the stable path is
`/dev/serial/by-id/usb-wch.cn_USB_Serial-if00`. Pass any device with `-d`.

## Usage

Defaults: device `/dev/ttyACM0`, baud **9600**, address **0x00** (this unit's
settings — the same ones the vendor GUI uses with reader type "UCM601").

```sh
./uhf_probe                       # confirm a reply, report baud/address
./uhf_probe -l 9600               # long passive capture at one baud
./uhf_scan  -d /dev/ttyACM0 -b 9600 -a 00
./uhf_console -d /dev/ttyACM0
```

### uhf_console

An interactive REPL exposing the **full R2000 command set** — type `help` for
the complete list. Highlights:

```
System    info  temp  reset  save  factory  checkant  setaddr  baud
RF/power  power  temppower  power4  power8  antenna  region  rflink
          cw  txtime  session  beep
Inventory one  scan  binv  inventory  stop  custom  sessioninv  multag
          buffer  bufferreset  buffercount  clearbuffer
Tag (G2)  dump  read  write  lock  kill  match  getmatch  select  getselect
GPIO      gpio  setgpio  keepalive
Raw       cmd <hex> [data]   raw <bytes...>
```

Most commands are get-or-set: with no argument they query, with an argument they
set (e.g. `power` prints the current dBm, `power 20` sets it; `beep off`,
`region 2 0 6` for ETSI, or `region custom 500 4 915000` for a user-defined
band). `addr <hex>` selects which reader address commands target.

Three ways to see tags: `one` reads the single tag's EPC (a bounded single-tag
read — most reliable for one tag); `scan [secs]` runs a live real-time inventory
and lists unique EPCs; `binv [secs]` uses the reader's buffered inventory
(dedups in the reader, reports a per-tag read count). Examples:

```
uhf> info                 -> reader: UCM601 v2.3
uhf> one                  -> read OK  pc=3000  EPC E2 00 00 ... 90 25
uhf> binv 2               buffered inventory: 1 unique tag, reads=90, freq=865300
uhf> dump                 read all four banks (EPC / TID / User / Reserved)
uhf> read 1 2 6           read 6 words from EPC bank @ word 2
uhf> write 1 2 <epchex>   write EPC (clone target); see below
uhf> beep off             silence buzzer, then `save` to persist
uhf> raw A0 03 00 72 EB   send exact bytes verbatim
```

### Cloning a tag

1. Place the **source** tag, `scan`, copy its EPC.
2. Place a **writable** tag, `write 1 2 <thatEPC>` (EPC bank, word address 2 —
   word 0 is the CRC, word 1 is the PC).

See [`docs/PROTOCOL.md`](docs/PROTOCOL.md) for the frame format and caveats
(writes are destructive; locked tags reject writes).

## Status

Verified end to end against the live unit (UCM601 v2.3) at 9600 baud / address 0:

- **Comms / config:** `info`, `power`, `antenna`, `temp`, `region`, etc. all
  round-trip (`GetFirmwareVersion` TX `A0 03 00 72 EB` → RX `A0 06 00 72 02 03
  01 E2`).
- **Inventory:** `one`, `scan` (real-time, deduped) and `binv` (buffered) all
  return the tag and leave the reader quiet afterward — no runaway, no wedge.
- **Read:** `dump` / `read` read every bank. Example card — EPC
  `E2000020091401632380​9025`, TID `E2003412013101000E5439AF`, user mem empty,
  reserved bank needs the access password.
- **Write / clone:** the `write`/`lock`/`kill` frames build correctly but are
  **not yet validated on a tag** (needs a writable target — see
  [`docs/TODO.md`](docs/TODO.md)).

> **RTS must be asserted** — this reader stays silent otherwise (the vendor app
> calls `setRTS()`); `serial.c` does this on open. Its inventory also *free-runs*
> (streams continuously until a Stop), so the tools send Stop and drain to a
> quiet state; the console additionally auto-calms on connect. `uhf_probe` stops
> as soon as it gets a valid reply so it never feeds the reader junk at other
> bauds.

## Files

```
include/serial.h  src/serial.c   POSIX termios serial layer
include/uhf.h     src/uhf.c      R2000 0xA0 build / parse / decode
src/probe.c                      uhf_probe
src/scan.c                       uhf_scan
src/console.c                    uhf_console
docs/PROTOCOL.md                 protocol reference (from the vendor app)
docs/TODO.md                     open items (clone/write, password recovery)
Makefile
```

## Disclaimer

This is an independent, clean-room reimplementation of the reader's serial
protocol, created for **interoperability** with hardware the author owns. The
protocol was recovered by observing and decompiling the vendor's own
application; this repository contains **no vendor code, binaries, or firmware** —
only an original C implementation.

"PiSwords", "UCM601", "Impinj", "R2000", "QinHeng" and other names are
trademarks of their respective owners, used here only nominatively to describe
hardware compatibility. No affiliation or endorsement is implied.

Use this only with RFID tags and readers you own or are authorised to access,
and in compliance with the laws of your jurisdiction. Provided **as is**,
without warranty of any kind.

## License

[MIT](LICENSE) © 2026 Hypnotize — see [`LICENSE`](LICENSE).
