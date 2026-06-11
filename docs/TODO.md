# TODO

## Verify the clone / copy write flow (needs a 2nd, writable tag)

The destructive tag-access commands (`write`, `lock`, `kill`) build correctly
and round-trip on the wire, but have **not** been exercised against a real tag
yet — at the time of writing only one tag was on hand, and cloning needs a
separate writable target so the source isn't overwritten.

When a blank/writable EPC Gen2 tag is available, verify the end-to-end clone:

1. Place the **source** tag, run `uhf> one` (or `binv`) and copy its EPC.
   Known source so far: `E2 00 00 20 09 14 01 63 23 80 90 25` (PC `3000`).
2. Place the **writable** target tag and write the EPC into the EPC bank:
   - `uhf> write 1 2 E2000020091401632380​9025`
     (bank `1` = EPC, word address `2` — word 0 is CRC, word 1 is PC).
   - For a 96-bit EPC the PC word should be `3000`; if the target's EPC length
     differs, also write the PC at word 1 so readers parse the right EPC length.
3. Re-scan the target and confirm its EPC now matches the source.

Checklist:
- [ ] `write` returns success (`0x10`); capture the reply bytes.
- [ ] Re-inventory shows the new EPC and correct PC/length.
- [ ] Behaviour on a **locked** tag — expect `0x40` access/password error;
      try `write ... <pw8>` with the access password.
- [ ] `lock` and `kill` (on a sacrificial tag only — `kill` is permanent).
- [ ] A `format`/`erase` convenience command (overwrite EPC + User to zeros,
      optionally reset passwords). Reversible since the EPC is recorded above;
      cannot touch the read-only TID.

## Password-recovery tool (Gen2 access/kill password)

There is **no "mfoc for UHF"** — EPC Gen2 has no broken cipher like MIFARE's
Crypto1. Reading EPC/TID is usually open (no key), and the 32-bit access/kill
passwords are only *cover-coded* (`password XOR RN16`), not encrypted. So the
realistic recovery paths are:

- **Eavesdropping** a legitimate reader↔tag access (XOR the captured RN16 with
  the masked password). NOT buildable with this reader — needs an RF sniffer
  (SDR/HackRF/USRP or Proxmark3). Out of scope for this hardware.
- **Brute force** over the air. Full 2^32 (~4.3e9) is infeasible online
  (months–years at this link speed). A **dictionary / smart brute** of weak
  passwords IS practical.

To build (reuses the existing `read`/`write`/`lock` plumbing + single-tag mode):
- [ ] `unlock <wordlist|range>` — try access passwords against a password-locked
      bank: attempt a read/write of a locked region; success ⇒ correct password.
      Start with defaults (`00000000`, `FFFFFFFF`, `11111111`, sequential,
      vendor patterns), then optional bounded brute range.
- [ ] `lockinfo` — probe each bank's lock state (open / pw-locked / permalocked)
      so you know whether a password is even needed.
- [ ] `log()` honestly that full 2^32 isn't attempted; report attempts/sec.
- [ ] Needs a **password-protected tag** to validate against (build + dry-run
      now, point at a locked tag later).

## Done

- [x] **Custom frequency region.** `region custom <spacingKHz> <channels>
      <startFreqKHz>` sends the user-defined-band form of `0x78`
      (`04 spacing(2) count(1) startFreq(3)`); `uhf_build_set_region_custom()`.
      Verified: set `custom 500 4 915000` → success, reads back as
      `custom spacing=500 channels=4 startFreq=915000 kHz`.
- [x] **RSSI calibration.** `uhf_rssi_dbm()` ports the vendor's
      `calculateRssi` + `para_B`/`para_C` lookup tables:
      `dBm = clamp(B*log10(magnitude/epcLen) + C, -90, 0)`, with rssiMode/
      hardwareMode taken from the raw field's top byte. Both inventory decoders
      fill `tag.rssi_dbm`; console (`scan`/`binv`/realtime) and the `uhf_scan`
      table show real dBm (verified live ≈ -80 dBm). Needs `-lm` (in Makefile).
- [x] **Named decode of config getters.** `get-region` (FCC/ETSI/CHN-1/CHN-2/
      custom + channels), `get-session` (S0–S3 / target A,B), `get-rflink`
      (profile table 0xD1/D6/D7/D9/DA/DB/DC/DD → FM0/Miller names) and
      `get-select` (enable/target/action/membank/pointer/truncation/mask) all
      print named fields instead of raw hex. Verified against vendor logs.
- [x] **Hardened the standalone `uhf_scan`.** It now kicks real-time inventory
      ONCE and reads the stream live (instead of re-firing it on top of the
      running stream), auto-calms on startup, re-kicks only after real silence,
      and `calm_reader()` hammers Stop on exit. Verified: after a 3 s scan +
      Ctrl-C the reader is quiet (0 B/s) and responds to get-version — no wedge.
- [x] **RTS must be asserted.** `serial.c` sets `TIOCM_RTS` on open. This was THE
      cause of the console intermittently going silent (`(no reply)`) while raw
      test tools replied — they set RTS, `serial.c` didn't. The reader needs RTS
      high (the vendor app calls `setRTS()`).
- [x] **Inventory no longer wedges the reader.** Root cause was missing RTS (so
      Stop wasn't heard) plus continuous free-running inventory saturating the
      9600 link. Now: `one` uses a bounded single-tag read; `scan` runs real-time
      inventory, dedups, then `calm_reader()` hammers Stop until the stream dies;
      the console auto-calms on startup; reply-matching ignores stale frames.
      Verified: after `scan`/`binv`, `info` replies immediately and a passive
      capture shows 0 B/s (fully stopped, no replug needed).
- [x] **`binv` — bounded buffered inventory** (clear → accumulate → Stop →
      read+clear buffer, `0x93`/`0x80`/`0x8C`/`0x91`). Dedups in the reader and
      reports per-tag read count + freq. Buffer-tag reply format reverse-
      engineered and decoded correctly (`invDataLen PC EPC CRC RSSI Freq ant cnt`).
- [x] **Read the whole card.** `dump` reads all 4 banks (auto single-tag mode);
      `read <bank> <wAddr> <wCnt> [pw8]` decodes PC/EPC/data (the 0x81 reply
      parser is implemented, no longer a hex dump). Verified: EPC
      `E2000020091401632380​9025`, TID `E2 00 34 12 01 31 01 00 0E 54 39 AF`,
      user mem empty/transient, reserved bank needs the access password (0x32).

## Caveat (firmware quirk, not a bug to fix)

If the reader is stressed hard (huge inventory bursts + many hammered Stops) it
can still get into a stuck state where it only emits stale stop-acks; only a USB
power-cycle clears that. Normal use (`one`/`scan`/`binv`/`dump`) no longer
triggers it.
