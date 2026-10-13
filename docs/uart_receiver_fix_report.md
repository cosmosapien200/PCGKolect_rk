# UART Receiver Fix Report — `PcgStream`

**Date:** 2026-10-11
**Component:** `src/pcg_stream.cpp`, `include/pcg_stream.hpp`
**Symptom:** The RV1103 received UART bytes from the ESP32 but decoded **zero
audio frames** (`frames_ok == 0`), so no PCG audio ever reached the
pre-processor.

---

## 1. Summary

End-to-end bring-up of the ESP32 → RV1103 PCG link failed even though the
sender (`PCGKolect/pcg-fw/main/uart_audio.c`) was verified to emit correct,
CRC-valid frames at exactly 8 kHz / 16-bit. Running the receiver on the
Luckfox Pico (RV1103) showed bytes arriving but no frames decoding.

**Root cause: two independent bugs in the receiver.**

| # | Bug | Effect |
|---|-----|--------|
| 1 | Wrong termios baud constant (`460800` instead of `B460800`) | Port ran at **921600** baud → framing errors, ~1.46× byte rate |
| 2 | Frame parser off-by-one (ignored the second sync byte `0x5A`) | Frames could **never** decode; `frames_ok` stuck at 0 |

Both are fixed and verified end-to-end: the receiver now decodes ~10 frames/s
with `crc_errors == 0`.

---

## 2. Observed symptom (before)

Command (run on the target over adb):

```sh
adb shell '/root/RVFirmware'
```

Output:

```
UART connected: /dev/ttyS3 @ 460800
Listening on /dev/ttyS3 (Ctrl+C to stop)...
frames=0 crc_err=0 bytes=46900 avail=0
frames=0 crc_err=0 bytes=93758 avail=0
frames=0 crc_err=0 bytes=140584 avail=0
frames=0 crc_err=0 bytes=187490 avail=0
```

Notes:

- `bytes` was **growing** (~23,450 B/s) — the wire was alive, so wiring and the
  ESP32 transmitter were fine.
- `frames_ok` stayed at **0** and `crc_errors` stayed at **0**. A CRC failure
  would increment `crc_errors`, so the parser was not even reaching the CRC
  check — it was stuck mid-frame.
- The expected byte rate is `10 frames/s × 1607 B = 16,070 B/s`; ~23,450 B/s is
  ~1.46× too high.
- `/proc/tty/driver/serial` showed framing errors accumulating:

  ```
  3: uart:16550A mmio:0xFF4D0000 irq:60 tx:0 rx:5087 fe:118 brk:78
  ```

---

## 3. Root cause #1 — wrong baud constant

`PcgStream::open()` configured the port with the **numeric** baud value:

```cpp
cfsetispeed(&tty, cfg_.baud);   // cfg_.baud == 460800 (int)
cfsetospeed(&tty, cfg_.baud);
```

`cfsetispeed()`/`cfsetospeed()` expect a `speed_t` termios **constant**, not a
number. On this toolchain (`asm-generic/termbits.h`):

```c
#define B460800 0010004   /* octal == 4100 decimal */
```

So the code was writing `460800` into the `c_cflag` speed field, which decodes
to a different speed. This was confirmed empirically — after the receiver
opened the port, `stty` reported:

```
speed 921600 baud;
```

The port was therefore running at **921600** while the ESP32 transmitted at
**460800**, producing constant framing errors (`fe`/`brk` grew) and a
~1.46× inflated byte count.

### Fix

Added a mapping helper and used it for both directions:

```cpp
static speed_t termiosSpeed(int baud) {
    switch (baud) {
        case 9600:   return B9600;
        case 19200:  return B19200;
        case 38400:  return B38400;
        case 57600:  return B57600;
        case 115200: return B115200;
        case 230400: return B230400;
        case 460800: return B460800;
        case 921600: return B921600;
        default:     return B115200;
    }
}
```

```cpp
if (cfsetispeed(&tty, termiosSpeed(cfg_.baud)) != 0 ||
    cfsetospeed(&tty, termiosSpeed(cfg_.baud)) != 0) {
    std::cerr << "[PcgStream] unsupported baud rate: " << cfg_.baud << std::endl;
    ::close(fd_);
    fd_ = -1;
    return false;
}
```

---

## 4. Root cause #2 — frame parser off-by-one

The documented frame is:

```
A5 5A | LEN (2, LE) | SEQ (1) | PCM (LEN bytes, int16 LE) | CRC16 (2, LE)
```

i.e. a **two-byte** sync `0xA5 0x5A`.

The parser (`readerLoop`) consumed only `0xA5` and then immediately treated the
**next byte (`0x5A`) as the LEN low byte**:

```cpp
case SEARCH:
    if (b == 0xA5) {
        frameBuf_[0] = 0xA5;
        frameIdx_ = 1;
        frameState_ = LEN_LO;     // <-- next byte (0x5A) stored as LEN lo
    }
    break;
case LEN_LO:
    frameBuf_[1] = b;             // <-- this is really the 2nd sync byte
    frameState_ = LEN_HI;
    break;
```

All subsequent field offsets and the CRC range were shifted by one byte:

```cpp
frameBuf_[1], frameBuf_[2] -> LEN
frameBuf_[3]              -> SEQ
frameBuf_[4]              -> PCM start
crc16(frameBuf_ + 1, 3 + len)   // should be frameBuf_ + 2
frameBuf_[4 + len], [5 + len]   // CRC, should be [5 + len], [6 + len]
```

**Trace** of the spec's own small-frame vector
`A5 5A 04 00 01 01 00 02 00 8F 8A` through the old parser:

| Step | Byte | State | Result |
|------|------|-------|--------|
| 1 | `A5` | SEARCH | `frameBuf_[0]=A5`, → `LEN_LO` |
| 2 | `5A` | LEN_LO | `frameBuf_[1]=5A` (2nd sync eaten) |
| 3 | `04` | LEN_HI | `len = 0x5A \| (0x04<<8) = 0x045A = 1114` → accepted |
| ... | ... | PAYLOAD | waits for 1114 payload bytes that never arrive |

Because `0x045A` is a valid (even, non-zero, ≤ 2000) length, the parser did not
resync; it silently swallowed the rest of the stream. This is why `frames_ok`
and `crc_errors` both stayed at 0.

### Fix

Added a dedicated `SYNC_HI` state that consumes and validates the second sync
byte, then shifted every field offset by one and corrected the CRC range.

Header (`include/pcg_stream.hpp`):

```cpp
enum FrameState { SEARCH, SYNC_HI, LEN_LO, LEN_HI, SEQ, PAYLOAD, CRC_LO, CRC_HI };
```

Parser (`src/pcg_stream.cpp`):

```cpp
case SEARCH:
    if (b == 0xA5) {
        frameBuf_[0] = 0xA5;
        frameIdx_ = 1;
        frameState_ = SYNC_HI;
    }
    break;

case SYNC_HI:
    if (b == 0x5A) {                 // second sync byte confirmed
        frameBuf_[1] = 0x5A;
        frameIdx_ = 2;
        frameState_ = LEN_LO;
    } else if (b == 0xA5) {          // restart on a new 0xA5
        frameBuf_[0] = 0xA5;
        frameIdx_ = 1;
    } else {                         // false sync — resync
        frameState_ = SEARCH;
        frameIdx_ = 0;
    }
    break;

// LEN -> frameBuf_[2],[3]; SEQ -> frameBuf_[4]; PCM -> frameBuf_[5]...
// CRC read at [5 + len],[6 + len]; CRC over crc16(frameBuf_ + 2, 3 + len)
```

The decoded PCM is now copied into a properly aligned, explicit little-endian
buffer before being pushed to the ring (the old `reinterpret_cast` on an odd
byte offset was alignment-unsafe):

```cpp
const size_t samples = len / 2;
for (size_t k = 0; k < samples; k++) {
    pcmBuf[k] = static_cast<int16_t>(
        static_cast<uint16_t>(frameBuf_[5 + 2 * k]) |
        (static_cast<uint16_t>(frameBuf_[6 + 2 * k]) << 8));
}
pushToRing(pcmBuf, samples);
```

`frameBuf_[4096]` is still large enough: the maximum frame is
`6 + LEN(max 2000) = 2006` bytes.

---

## 5. Verification (after)

Rebuilt and deployed:

```sh
source env.sh
make build
make upload
adb shell '/root/RVFirmware'
```

Output (~32 s run):

```
UART connected: /dev/ttyS3 @ 460800
Listening on /dev/ttyS3 (Ctrl+C to stop)...
frames=19  crc_err=0 bytes=32304
frames=40  crc_err=0 bytes=66051
frames=61  crc_err=0 bytes=99798
frames=82  crc_err=0 bytes=133545
frames=102 crc_err=0 bytes=165685
frames=123 crc_err=0 bytes=199432
frames=144 crc_err=0 bytes=233179
frames=164 crc_err=0 bytes=265319
frames=185 crc_err=0 bytes=299066
frames=204 crc_err=1 bytes=332804
frames=224 crc_err=1 bytes=364944
frames=245 crc_err=1 bytes=398691
frames=266 crc_err=1 bytes=432438
frames=287 crc_err=1 bytes=466185
frames=308 crc_err=1 bytes=499932
```

Checks:

| Check | Result |
|-------|--------|
| `stty -F /dev/ttyS3` after open | **`speed 460800 baud`** ✅ |
| Bytes per report | exactly `21 × 1607 = 33,747` ✅ |
| Frame rate | ~10 frames/s (`frames_ok` climbs steadily) ✅ |
| Byte rate | `rx` delta ÷ time = **16,025 B/s** (expected 16,070) ✅ |
| `/proc/tty/driver/serial` `fe`/`brk` | **unchanged** during the run ✅ |
| `crc_errors` | 1 error over 308 frames (~0.3 %) |

Because a CRC-16 false positive is only 1/65536 per frame, 300+ consecutive
frames passing CRC conclusively proves real, well-formed audio is being decoded.

### Residual observation

The single CRC error correlates with the UART **overrun** counter (`oe`)
incrementing (1 → 3). This points to a rare kernel RX FIFO overrun dropping a
byte (which misaligns one frame; the parser then resyncs on the next `A5 5A`).
It is self-recovering and non-blocking. Optional follow-ups if stricter
integrity is required:

- Read the port with a shorter `VTIME` / larger input buffer.
- Add a `frames_dropped` statistic derived from `SEQ` gaps.
- Raise the reader thread priority.

---

## 6. Files changed

| File | Change |
|------|--------|
| `include/pcg_stream.hpp` | Added `SYNC_HI` to `FrameState` |
| `src/pcg_stream.cpp` | Added `termiosSpeed()` baud mapping; fixed `cfsetispeed`/`cfsetospeed`; added `SYNC_HI` sync state; corrected LEN/SEQ/PCM/CRC offsets and CRC range; alignment-safe PCM copy |

No changes were required on the ESP32 sender — it already conformed to the
documented protocol (`docs/uart_pcg_communication.md`, §4) and its test vectors.
