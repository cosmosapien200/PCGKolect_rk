# Development Log

## 2026-10-13 — Display Troubleshooting: Title Cutoff Fix

### Problem

After integrating u8g2-linux, the title "PCGKolect RV1103" was partially cut off at the top of the SSD1306 display. The first line was barely visible or completely missing.

### Root Cause

**u8g2 uses a baseline coordinate system, not top-left.** In u8g2, the `y` parameter in `drawStr(x, y, text)` is the **text baseline** (the imaginary line the text sits on), NOT the top edge of the bounding box.

The original code used:
```cpp
u8g2.drawStr(2, 8, "PCGKolect RV1103");  // baseline at y=8
```

With the `u8g2_font_6x13_tr` font (13 pixels tall), the text extends **upward** from the baseline. So at y=8, the top of the text was at approximately y = 8 - 13 = **-5**, which is off-screen. Only the bottom portion of the letters was visible.

Additionally, the separator line was drawn at y=14, which overlapped with the visible bottom portion of the title text.

### Fix

```cpp
u8g2.drawStr(2, 14, "PCGKolect RV1103");  // baseline at y=14
u8g2.drawHLine(0, 16, 128);                // separator below title
```

With baseline at y=14, the 13px-tall title spans y=1 to y=14 — fully visible. The separator at y=16 sits cleanly below.

All stat row y-coordinates were adjusted accordingly (28, 38, 48, 58 instead of 26, 36, 46, 56).

### u8g2 Y-Coordinate Reference

| Element | Font | Font Height | Baseline Y | Visible Y Range |
|---------|------|-------------|------------|-----------------|
| Title | 6x13_tr | 13px | 14 | 1 – 14 |
| Separator | — | — | — | y=16 |
| Stats | 5x7_tf | 7px | 28, 38, 48, 58 | 21–28, 31–38, 41–48, 51–58 |

### Files Changed
- `src/display.cpp` — adjusted y-coordinates for all text elements

---



## 2026-10-13 — Replace Hand-Rolled SSD1306 Driver with u8g2-linux

### Milestone

- Scrapbed the hand-rolled SSD1306 I2C driver (repeated-start transaction issues, gibberish output)
- Integrated `u8g2-linux` library for proven, well-tested SSD1306 support
- Display shows "PCGKolect RV1103" title + UART stats (frames, CRC errors, bytes, buffer)
- Binary size optimized: **12 MB → 125 KB** via `-ffunction-sections -fdata-sections -Wl,--gc-sections`
- Display init is non-fatal: UART/audio pipeline continues if display fails

### Implementation Notes

- **Copied `u8g2-linux/lib/u8g2-arm-linux/`** → `lib/u8g2/` (298 files: cppsrc, csrc, drivers, port)
- **`include/display.hpp`** — Display class with Config struct (I2C bus, address, reset pin)
- **`src/display.cpp`** — Wraps `U8G2_SSD1306_128X64_NONAME_F_HW_I2C`:
  - `init()` — calls `initI2cHw(3)`, `setI2CAddress(0x78)`, `begin()`
  - `drawStatus()` — draws title bar, separator, 4 stat lines, CRC warning indicator, heartbeat dot
  - `flush()` — calls `sendBuffer()` (full frame buffer → one I2C transaction)
- **`CMakeLists.txt`** — GLOBs all u8g2 sources, adds include dirs, compile defs, linker flags
- **`src/main.cpp`** — Display init before UART; display flush every 200ms in main loop
- u8g2-linux uses `I2C_RDWR` ioctl with `struct i2c_msg` — the correct Linux I2C API that avoids repeated-start issues

### Verification

```
RV1103 Firmware for PCG Kollect
Display initialized on I2C bus 3
UART connected: /dev/ttyS3 @ 460800
Listening on /dev/ttyS3 (Ctrl+C to stop)...
frames=19 crc_err=0 bytes=33633 avail=0
```

- Display renders "PCGKolect RV1103" header + stats every 200ms
- I2C3 at 100 kHz (device tree configured)
- UART audio pipeline running with 0 CRC errors
- Binary: 125 KB ELF (ARM 32-bit, dynamically linked)

### Files Changed

| File | Change |
|------|--------|
| `lib/u8g2/` | **New** — u8g2-linux library (298 files) |
| `include/display.hpp` | **Replaced** — u8g2 wrapper header |
| `src/display.cpp` | **Replaced** — u8g2-based SSD1306 driver |
| `src/main.cpp` | Added display init + periodic status flush |
| `CMakeLists.txt` | Added u8g2 sources, include dirs, compile/link flags |

---

## 2026-10-13 — SSD1306 OLED Display Working on I2C3

### Milestone

- Added SSD1306 OLED display support (128×64, I2C, address 0x3C)
- "PCGK" title displayed on startup
- UART stats (frames, CRC errors, buffer) updated on display every 2 seconds
- Removed `test_audio_record.cpp` test binary

### Implementation

- **`include/display.hpp` / `src/display.cpp`** — New I2C SSD1306 driver
  - Opens `/dev/i2c-3`, sets I2C slave address 0x3C
  - Full SSD1306 init sequence (clock, multiplex, charge pump, addressing, contrast)
  - 5×7 bitmap font for "PCGK" characters (embedded C arrays, no external deps)
  - `drawChar()` / `drawString()` — bitmap blitting to page-mode frame buffer
  - `flush()` — sends 8 pages × 128 bytes via I2C (0x40 data mode)
  - `isOpen()` — safe check for display availability (firmware continues without display)
- **`src/main.cpp`** — wired display init before UART, periodic status updates in main loop
- **`CMakeLists.txt`** — removed `test_audio_record` target, added `display.cpp`
- **`Makefile`** — removed `test-*` targets

### UI Layout (128×64)

```
┌──────────────────────────────┐
│   P C G K                    │  ← 5×7 font, centered at (24, 1)
│                              │
│  Frames: 1234                │  ← 5×7 font at (2, 23)
│  CRC err: 0                  │  ← 5×7 font at (2, 33)
│  Buf: 0                      │  ← 5×7 font at (2, 43)
└──────────────────────────────┘
```

### Verification

```
RV1103 Firmware for PCG Kollect
Display OK              ← I2C init succeeded
UART connected: /dev/ttyS3 @ 460800
Listening on /dev/ttyS3 (Ctrl+C to stop)...
frames=20 crc_err=0 bytes=33747 avail=0
```

- Display initialized, "PCGK" renders on OLED
- I2C3 at 100 kHz (configured via luckfox-config)
- UART audio pipeline still running normally alongside display

### Files Changed

| File | Change |
|------|--------|
| `include/display.hpp` | **New** — Display class header |
| `src/display.cpp` | **New** — SSD1306 I2C driver |
| `src/main.cpp` | Added display init + status updates |
| `src/test_audio_record.cpp` | **Deleted** |
| `CMakeLists.txt` | Removed test target, added display.cpp |
| `Makefile` | Removed test-* targets |

---

### Milestone

- End-to-end audio capture pipeline verified: ESP32 → UART → RV1103 → WAV file
- Built `test_audio_record.cpp` — records 5 seconds of PCG audio and saves as a standard WAV file
- **0 CRC errors** over 40,000 samples (50 frames) — clean signal
- WAV file confirmed valid: RIFF WAVE, 16-bit PCM, mono, 8000 Hz, 5.00 seconds

### Implementation

- Added `src/test_audio_record.cpp` — separate test binary (easy to remove)
  - Opens `/dev/ttyS3` @ 460800 baud
  - Reads 40,000 samples (5s × 8kHz) in 800-sample chunks
  - Writes standard 44-byte WAV header + raw PCM to `/root/recordings/recording.wav`
  - Prints stats then exits cleanly
- Added CMake target `test_audio_record` (links `pcg_stream.cpp`)
- Added Makefile targets: `test-build`, `test-upload`, `test-run`, `test`
- Updated `.gitignore` to exclude `recordings/` directory
- Created `AGENTS.md` — comprehensive agent development guide
- Created `DEVLOG.md` — development log (this file)
- Added RKNN SDK documentation in `docs/rknn_rv1103_docs/` (RV1103-focused)

### Verification

```
[test] UART connected: /dev/ttyS3 @ 460800
[test] Recording 5s (40000 samples)...
[test] Saved 40000 samples to /root/recordings/recording.wav (80044 bytes)
[test] Stats: frames_ok=50  crc_errors=0  bytes_read=81466
```

WAV file verification:
- `file` output: `RIFF (little-endian) data, WAVE audio, Microsoft PCM, 16 bit, mono 8000 Hz`
- Header parsed: RIFF size=80036, fmt=16, audio=1(PCM), channels=1, rate=8000, bits=16
- Duration: 80000 bytes / 16000 B/s = **5.00 seconds**

### Test with noise

- Recorded audio was tested with external noise sources (ambient sounds near the PCG sensor)
- The WAV file contains audible signal content — the UART pipeline reliably transfers audio including dynamic content
- No sample loss, no CRC errors, no buffer overflows during recording

### Files Changed

| File | Change |
|------|--------|
| `src/test_audio_record.cpp` | **New** — 5s WAV capture test |
| `src/pcg_stream.cpp` | Unchanged (used by test) |
| `CMakeLists.txt` | Added `test_audio_record` target |
| `Makefile` | Added `test-*` targets |
| `.gitignore` | Added `recordings/` |
| `AGENTS.md` | **New** — agent development guide |
| `DEVLOG.md` | **New** — development log |
| `docs/uart_pcg_communication.md` | **New** — UART protocol spec |
| `docs/uart_receiver_fix_report.md` | **New** — bug fix report |
| `docs/rknn_rv1103_docs/` | **New** — RKNN SDK docs (RV1103-focused) |

---

## 2026-10-13 — Project Initialization

### Milestone

- RV1103 firmware scaffold created for PCG data collection
- `PcgStream` module: UART frame parser with CRC16-CCITT-FALSE, ring buffer, async reader thread
- `PcgPreprocessor` module: DC-block → bandpass (20-500 Hz) → RMS normalize
- Main loop: 100ms frame reads, preprocessing, ready for AI model integration

### Architecture

- ESP32-S3 streams framed 8 kHz/16-bit PCM over UART3 @ 460800 baud
- RV1103 parses frames, buffers in ring buffer, feeds preprocessor
- Output: float [-1, 1] samples ready for spectrogram + ML inference

### Files Changed

| File | Change |
|------|--------|
| `include/pcg_stream.hpp` | **New** — UART reader class |
| `src/pcg_stream.cpp` | **New** — frame parser, CRC, ring buffer |
| `include/pcg_preprocessor.hpp` | **New** — audio filter chain |
| `src/pcg_preprocessor.cpp` | **New** — biquad filters, DC blocker |
| `src/main.cpp` | **New** — main loop |
| `src/system_info.cpp` | **New** — board info |
| `CMakeLists.txt` | **New** — build definition |
| `Makefile` | **New** — build/deploy targets |
| `env.sh` | **New** — toolchain setup |
| `cmake/rv1103-toolchain.cmake` | **New** — cross-compile config |

---
