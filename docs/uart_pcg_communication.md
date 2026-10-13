# UART PCG Communication Module

Documentation for the UART link that streams PCG (phonocardiogram) audio from an
**ESP32** to the **Luckfox Pico (RV1103)** board, plus the receiver and
pre-processing modules implemented in this firmware.

---

## 1. Overview

The RV1103 receives a single **mono** PCG audio stream over **UART3
(`/dev/ttyS3`)**. Incoming bytes are parsed into audio frames, buffered, and
passed through a pre-processing chain (DC-block → bandpass → normalize) to
produce AI-ready floating-point samples.

```
 ESP32                                   RV1103 (Luckfox Pico)
┌───────────────┐                       ┌──────────────────────────────┐
│  PCG sensor   │                       │                              │
│  ADC @ 8 kHz  │  UART3 (460800 8N1)   │  PcgStream                   │
│  16-bit mono  │ ────────────────────► │   ├─ frame parser (CRC)      │
│  frame builder│   A5 5A | LEN | ...   │   └─ ring buffer             │
└───────────────┘                       │        │                     │
                                        │        ▼                     │
                                        │  PcgPreprocessor             │
                                        │   DC-block → bandpass → norm │
                                        │        │                     │
                                        │        ▼                     │
                                        │  AI model (future)           │
                                        └──────────────────────────────┘
```

### Source files

| File | Role |
|------|------|
| `include/pcg_stream.hpp` / `src/pcg_stream.cpp` | UART transport, framing, buffering |
| `include/pcg_preprocessor.hpp` / `src/pcg_preprocessor.cpp` | Audio pre-processing |
| `src/main.cpp` | Wiring / main loop |

---

## 2. Link Settings

| Parameter | Value |
|-----------|-------|
| **Port (RV1103)** | `/dev/ttyS3` (UART3) |
| **Baud rate** | **460800** |
| **Data bits** | 8 |
| **Parity** | None |
| **Stop bits** | 1 |
| **Flow control** | None |
| **Mode** | Raw (non-canonical, no echo) |

### Bandwidth check

UART carries **10 bits per byte** (1 start + 8 data + 1 stop).

| Baud | Usable throughput |
|------|-------------------|
| 115200 | 11,520 B/s |
| **460800** | **46,080 B/s** |
| 921600 | 92,160 B/s |

Audio payload = 8,000 samples/s × 2 bytes = **16,000 B/s**.

> 16,000 B/s fits in 460800 (≈ 35 % util) but **does not fit in 115200**
> (needs 11,520 B/s max). **460800 is required** for 8 kHz / 16-bit.

---

## 3. Audio Format

| Property | Value |
|----------|-------|
| **Sample rate** | 8,000 Hz |
| **Resolution** | 16-bit signed (`int16`) |
| **Channels** | 1 (mono) |
| **Endianness** | Little-endian |
| **Range** | `-32768 … +32767` |

PCG content of interest is roughly 20–500 Hz, so 8 kHz gives generous
headroom and matches standard audio/ML tooling.

---

## 4. Packet Format

Audio is sent in fixed-size frames, ~**10 frames per second**.

### 4.1 Frame layout

```
┌────────┬────────┬───────┬──────────────────┬─────────┐
│ SYNC   │ LEN    │ SEQ   │ PCM DATA         │ CRC16   │
│ 2 B    │ 2 B LE │ 1 B   │ LEN bytes        │ 2 B LE  │
│ A5 5A  │        │ wraps │ int16 LE         │ CCITT   │
└────────┴────────┴───────┴──────────────────┴─────────┘
```

| Field | Offset | Size | Description |
|-------|--------|------|-------------|
| `SYNC` | 0 | 2 | Constant `0xA5 0x5A`. Resync anchor. |
| `LEN` | 2 | 2 | Payload length in **bytes**, little-endian. Must be **even**, **non-zero**, **≤ 2000**. |
| `SEQ` | 4 | 1 | Frame counter. Increments each frame; wraps at 256. Used to detect dropped frames. |
| `PCM` | 5 | `LEN` | `LEN/2` little-endian `int16` samples. |
| `CRC16` | 5 + `LEN` | 2 | CRC16-CCITT-FALSE over `LEN + SEQ + PCM`, little-endian. |

### 4.2 Standard 100 ms frame

| | |
|---|---|
| Samples per frame | **800** |
| Payload (`LEN`) | **1600** bytes |
| Total frame size | 2 + 2 + 1 + 1600 + 2 = **1607 bytes** |
| Payload rate | 800 samples × 10 frames/s = 8 kHz |
| Frame rate | ~10 frames/s |

### 4.3 CRC16 rules

| Property | Value |
|----------|-------|
| Algorithm | CRC16-CCITT-FALSE (XMODEM style) |
| Polynomial | `0x1021` |
| Initial value | `0xFFFF` |
| Reflect in / out | No / No |
| XOR out | `0x0000` |
| Coverage | bytes `LEN(lo) … last PCM byte` (i.e. `3 + LEN` bytes) |
| Stored as | 2 bytes, **little-endian** |

### 4.4 CRC reference implementation (C)

```c
uint16_t crc16_ccitt_false(const uint8_t *data, size_t len) {
    uint16_t crc = 0xFFFF;
    for (size_t i = 0; i < len; i++) {
        crc ^= (uint16_t)data[i] << 8;
        for (int j = 0; j < 8; j++) {
            if (crc & 0x8000)
                crc = (uint16_t)((crc << 1) ^ 0x1021);
            else
                crc = (uint16_t)(crc << 1);
        }
    }
    return crc;
}
```

### 4.5 Test vectors

**Known-good algorithm check** (must equal `0x29B1`):

```
crc16_ccitt_false("123456789") == 0x29B1
```

**Small complete frame** — `LEN = 4`, `SEQ = 1`, samples `[1, 2]`:

```
body (LEN+SEQ+PCM) : 04 00 01 01 00 02 00
CRC16              : 0x8A8F  ->  LE bytes  8F 8A
full frame         : A5 5A 04 00 01 01 00 02 00 8F 8A
```

**Standard 100 ms frame of silence** — `LEN = 1600`, `SEQ = 0`, all-zero PCM:

```
LEN bytes          : 40 06   (0x0640 = 1600)
CRC16              : 0x5E22
```

---

## 5. RV1103 Receiver — `PcgStream`

`PcgStream` owns the serial port. On `open()` it configures the port, starts a
**background reader thread**, and decodes frames into a **ring buffer**.
Consumer code never blocks on UART timing, so slow AI inference cannot cause
overruns.

### 5.1 API

```cpp
#include "pcg_stream.hpp"

class PcgStream {
public:
    struct Config {
        const char* device       = "/dev/ttyS3";
        int         baud         = 460800;
        size_t      ring_samples = 32000;   // ~4 s @ 8 kHz
    };

    struct Stats {
        uint64_t frames_ok  = 0;   // valid frames decoded
        uint64_t crc_errors = 0;   // frames with bad CRC or bad LEN
        uint64_t bytes_read = 0;   // raw bytes pulled from UART
    };

    bool   open(const Config& cfg);                         // start
    void   close();                                         // stop + join
    size_t read(int16_t* out, size_t count, int timeout_ms); // pull samples
    size_t available() const;                               // samples buffered
    Stats  stats() const;                                   // diagnostics
};
```

### 5.2 Behaviour notes

- **`read()`** copies **up to** `count` samples and returns the number copied.
  It blocks until `count` samples are buffered or `timeout_ms` elapses.
  A return value `< count` means a timeout (partial or no data).
- **Ring buffer fullness:** if the consumer falls behind and the ring fills,
  the **oldest** samples are dropped to make room (bounded memory, no growth).
- **Resynchronisation:** if `LEN` is malformed (zero, odd, or > 2000) or CRC
  fails, the parser returns to `SEARCH` and scans forward for the next
  `0xA5 0x5A` sync.
- **Sync handling:** the parser consumes **both** sync bytes. After `0xA5` it
  requires `0x5A` before reading `LEN`; anything else resyncs.
- **Baud:** `cfsetispeed`/`cfsetospeed` must be given the termios constant
  `B460800`, **not** the number `460800`. Passing the raw number silently sets
  the wrong speed (observed: 921600). See
  [uart_receiver_fix_report.md](uart_receiver_fix_report.md).
- **`SEQ`** gaps indicate dropped frames; the counter is currently surfaced via
  the raw field but not yet aggregated (future: a `frames_dropped` stat).

---

## 6. Pre-processing — `PcgPreprocessor`

Converts `int16` PCM to **`float`** in `[-1, 1]` and applies a fixed filter
chain. Filter state persists across calls, so it must be fed contiguous audio.

### 6.1 Chain

1. **int16 → float**: `x[i] = in[i] / 32768.0f`
2. **DC block** — 1-pole high-pass at 1 Hz
   `y[n] = x[n] − x[n−1] + r·y[n−1]`, `r = exp(−2π·fc/fs)`
3. **Low-pass** — 2nd-order Butterworth at `high_hz` (500 Hz), `Q = 1/√2`
4. **High-pass** — 2nd-order Butterworth at `low_hz` (20 Hz), `Q = 1/√2`
5. **RMS normalize** — scales each buffer to `target_rms` (0.5)

### 6.2 API

```cpp
#include "pcg_preprocessor.hpp"

class PcgPreprocessor {
public:
    struct Config {
        int   sample_rate = 8000;
        float low_hz      = 20.0f;    // bandpass low edge
        float high_hz     = 500.0f;   // bandpass high edge
        bool  normalize   = true;
        float target_rms  = 0.5f;
    };

    explicit PcgPreprocessor(const Config& cfg);

    void process(const int16_t* in, float* out, size_t count);
    void reset();                                               // clear filter state
};
```

`in` and `out` are different types/dimensions (`int16` → `float`), so they must
be separate buffers. Both must hold at least `count` elements.

---

## 7. Usage Example

```cpp
#include "pcg_stream.hpp"
#include "pcg_preprocessor.hpp"

#include <vector>

static constexpr size_t FRAME = 800;   // 100 ms @ 8 kHz

int main() {
    PcgStream stream;
    PcgStream::Config sc;
    sc.device = "/dev/ttyS3";
    sc.baud   = 460800;
    if (!stream.open(sc)) return 1;

    PcgPreprocessor::Config pc;
    pc.sample_rate = 8000;
    pc.low_hz      = 20.0f;
    pc.high_hz     = 500.0f;
    pc.normalize   = true;
    PcgPreprocessor pre(pc);

    std::vector<int16_t> raw(FRAME);
    std::vector<float>   proc(FRAME);

    while (true) {
        size_t n = stream.read(raw.data(), FRAME, 200);
        if (n == 0) continue;                 // timeout, no data
        pre.process(raw.data(), proc.data(), n);

        // proc[0..n) is AI-ready float audio.
        // TODO: spectrogram + model inference here.
    }
}
```

---

## 8. Diagnostics & Troubleshooting

`PcgStream::stats()` counters:

| Field | Meaning |
|-------|---------|
| `bytes_read` | Raw bytes received on the UART |
| `frames_ok` | Frames that passed CRC |
| `crc_errors` | Frames rejected (bad CRC / bad length) |

### Symptom table

| Symptom | Likely cause | Fix |
|---------|--------------|-----|
| `bytes_read` grows, `frames_ok == 0` | Wrong baud / wrong protocol on wire | Set sender to **460800 8N1** and use the frame format in §4 |
| `bytes_read` stays 0 | Sender idle, wrong wiring (TX/RX), or UART3 pinmux off | Check TX↔RX crossover and device-tree UART3 enablement |
| `crc_errors` climbing | Noise, baud drift, or misaligned `LEN`/`SEQ` | Verify baud on both ends; check CRC coverage = `LEN+SEQ+PCM` |
| `bytes_read` grows, `frames_ok == 0`, `crc_errors == 0` | Parser stuck mid-frame (e.g. baud constant bug) | Ensure `cfsetispeed` uses `B460800`, not `460800` — see [fix report](uart_receiver_fix_report.md) |
| `fe`/`brk` climb in `/proc/tty/driver/serial` | Wrong termios baud constant | Use `B460800` (the constant), not the number `460800` |
| `open()` fails | `/dev/ttyS3` missing or busy | Confirm `ls /dev/ttyS*`; ensure nothing else holds the port |
| Framing errors in `/proc/tty/driver/serial` (`fe`, `brk`) | Baud mismatch | Match both ends to 460800 |

### Checking the live port (host / board)

```sh
# Is UART3 present?
adb shell ls -l /dev/ttyS3

# Serial driver stats (rx/tx counters + framing errors)
adb shell cat /proc/tty/driver/serial
```

---

## 9. Quick Reference

### Constants (RV1103 side)

| Constant | Value | Where |
|----------|-------|-------|
| Device | `/dev/ttyS3` | `PcgStream::Config::device` |
| Baud | `460800` | `PcgStream::Config::baud` |
| Ring buffer | `32000` samples (4 s) | `PcgStream::Config::ring_samples` |
| Frame size | `800` samples (100 ms) | `main.cpp` `FRAME_SAMPLES` |
| Sample rate | `8000` Hz | `PcgPreprocessor::Config::sample_rate` |
| Bandpass | `20 … 500` Hz | `low_hz`, `high_hz` |
| Normalize target | `0.5` RMS | `target_rms` |

### Protocol constants

| Constant | Value |
|----------|-------|
| Sync | `0xA5 0x5A` |
| Max `LEN` | 2000 bytes |
| `LEN` parity | even, non-zero |
| CRC | CRC16-CCITT-FALSE (`0x1021`, init `0xFFFF`) |
| CRC stored | little-endian |
| PCM | `int16`, little-endian, mono |

### ESP32 sender checklist

- [ ] UART set to **460800 baud, 8N1, no flow control**
- [ ] Sample PCG at **8 kHz, 16-bit signed, mono**
- [ ] Emit a frame every **100 ms** (800 samples, `LEN = 1600`)
- [ ] Prefix with `A5 5A`, include `LEN` (LE), `SEQ`
- [ ] Append `CRC16-CCITT-FALSE` (LE) over `LEN + SEQ + PCM`
- [ ] Verify against the test vectors in §4.5
