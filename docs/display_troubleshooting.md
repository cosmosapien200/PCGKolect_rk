# Display Troubleshooting — Hand-Rolled Driver → u8g2-linux

## Timeline

| Date | Event |
|------|-------|
| 2026-10-13 (morning) | Hand-rolled SSD1306 I2C driver produces gibberish / no output |
| 2026-10-13 (afternoon) | Switch to u8g2-linux library — display works |
| 2026-10-13 (evening) | Fix title cutoff — u8g2 baseline coordinate system |

---

## Phase 1: Hand-Rolled SSD1306 Driver (Failed)

### What we tried

A custom I2C driver was written from scratch:
- Opens `/dev/i2c-3`, sets slave address 0x3C via `I2C_SLAVE` ioctl
- Sends full SSD1306 init sequence (multiplex ratio, display offset, charge pump, memory addressing mode, contrast, display on)
- Uses page-mode rendering: 8 pages × 128 bytes per flush
- 5×7 bitmap font embedded as C arrays

### Symptoms

- **Gibberish output** — random garbage pixels on the OLED
- **No text visible** — init sequence appeared to "succeed" (no I2C errors) but nothing rendered correctly

### Root Cause Analysis

The hand-rolled driver used **separate I2C writes** for command and data:

```cpp
// Command mode
msg[0].addr = 0x3C;
msg[0].buf  = &cmd;
msg[0].len  = 1;
ioctl(fd, I2C_RDWR, &i2c_msg);

// Data mode (separate transaction)
msg[0].addr = 0x3C;
msg[0].buf  = pixel_data;
msg[0].len  = 128;
ioctl(fd, I2C_RDWR, &i2c_msg);
```

**The problem:** SSD1306 uses a **control byte** to switch between command and data modes within a single I2C transaction. The correct approach requires:

1. Start condition → address (0x3C) + write
2. Control byte `0x00` (command mode)
3. Command bytes
4. **Repeated start** (no stop condition!)
5. Address (0x3C) + write
6. Control byte `0x40` (data mode)
7. Pixel data bytes

The hand-rolled driver issued **two separate I2C transactions** with a stop condition between them. The SSD1306 interprets the stop as "end of burst" and resets its internal command/data pointer. On the next transaction, it expects a command byte but receives pixel data — resulting in gibberish.

### Why `i2cdetect -y 3` worked but rendering didn't

`i2cdetect` sends a simple **I2C read probe** (start + address + read + stop). This doesn't require the SSD1306's command/data control byte mechanism — it's just checking if an ACK exists at address 0x3C. So the device was confirmed present, but the rendering protocol was wrong.

---

## Phase 2: u8g2-linux Integration (Success)

### Decision

Abandon the hand-rolled driver and integrate **u8g2-linux** — a proven, well-tested graphics library with SSD1306 support.

### Why u8g2-linux?

1. **Correct I2C implementation** — uses `I2C_RDWR` ioctl with `struct i2c_msg[]` array, sending command control byte + commands + data control byte + pixel data in a **single transaction** with repeated start
2. **Already tested** — used by thousands of Arduino/embedded projects
3. **Full API compatibility** — `drawStr()`, `setFont()`, `sendBuffer()` are standard u8g2 calls
4. **Swappable driver** — can swap the low-level I2C implementation later without changing application code
5. **No external deps** — static compilation, all sources included

### Build Integration

#### Source tree

```
lib/u8g2/                          # Copied from u8g2-linux/lib/u8g2-arm-linux/
├── cppsrc/                        # C++ wrapper (U8g2lib.h/cpp, U8x8lib.h/cpp)
├── csrc/                          # C core (u8g2_*.c, u8x8_*.c, drivers)
├── drivers/                       # Linux I2C, GPIO, SPI, PWM drivers
└── port/                          # Porting layer (u8g2port.c)
```

298 files total.

#### CMakeLists.txt changes

```cmake
# GLOB all u8g2 sources
file(GLOB U8g2_SRC
    ${U8G2PATH}/cppsrc/*.cpp
    ${U8G2PATH}/cppsrc/*.h
    ${U8G2PATH}/csrc/*.c
    ${U8G2PATH}/csrc/*.h
    ${U8G2PATH}/drivers/*.c
    ${U8G2PATH}/drivers/*.h
    ${U8G2PATH}/port/*.c
    ${U8G2PATH}/port/*.h
)
target_sources(${PROJECT_NAME} PRIVATE ${U8g2_SRC})

# Include paths
target_include_directories(${PROJECT_NAME} PRIVATE
    ${U8G2PATH}/cppsrc
    ${U8G2PATH}/csrc
    ${U8G2PATH}/drivers
    ${U8G2PATH}/port
)

# Required defines for Linux I2C + cdev GPIO
set(CMAKE_C_FLAGS   "${CMAKE_C_FLAGS} -D__ARM_LINUX__ -DPERIPHERY_GPIO_CDEV_SUPPORT=1")
set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -D__ARM_LINUX__ -D__ARM_LINUX__ -DPERIPHERY_GPIO_CDEV_SUPPORT=1")
```

#### Binary size optimization

Initial build: **12 MB** (all u8g2 device drivers included, no linker stripping)

Adding section-based garbage collection:

```cmake
set(CMAKE_C_FLAGS   "${CMAKE_C_FLAGS} -ffunction-sections -fdata-sections")
set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -ffunction-sections -fdata-sections")
set(CMAKE_EXE_LINKER_FLAGS "${CMAKE_EXE_LINKER_FLAGS} -Wl,--gc-sections")
```

Result: **125 KB** (96× reduction — unused SSD1306 variants, ST7565 drivers, fonts, etc. all stripped)

### Display class design

```cpp
// include/display.hpp
class Display {
public:
    struct Config {
        int  i2c_bus   = 3;
        uint8_t i2c_addr = 0x78;  // 0x3C << 1
        int  reset_pin = -1;       // U8X8_PIN_NONE
    };

    Display() = default;
    explicit Display(const Config& cfg);
    bool init();
    void drawStatus(int frames_ok, int crc_errors, int bytes_read,
                    int buffer_available, int crc_rate);
    void flush();
    void clear();
    bool isInitialized() const;
};
```

### Usage in main.cpp

```cpp
// Init before UART (non-fatal)
Display::Config cfg;
cfg.i2c_bus = 3;
cfg.i2c_addr = 0x78;
Display disp(cfg);
if (!disp.init()) {
    std::cerr << "Display init failed" << std::endl;
    // Continue — UART still works
}

// In main loop, every 200ms
auto s = stream.stats();
disp.drawStatus(s.frames_ok, s.crc_errors, s.bytes_read,
                stream.available(), crc_rate);
disp.flush();
```

---

## Phase 3: Title Cutoff Fix

### Problem

After u8g2-linux integration, the title "PCGKolect RV1103" was partially cut off at the top of the display.

### Investigation

The original code:
```cpp
u8g2.setFont(u8g2_font_6x13_tr);
u8g2.drawStr(2, 8, "PCGKolect RV1103");  // y=8
u8g2.drawHLine(0, 14, 128);               // separator at y=14
```

**Key discovery: u8g2 uses a baseline coordinate system.**

In u8g2, the `y` parameter is the **text baseline** — the imaginary line that letters "sit on." Text extends **upward** from the baseline by the font's height.

For `u8g2_font_6x13_tr` (13 pixels tall):
- Baseline at y=8 → text spans y = 8 - 13 = **-5** to y = 8 → top 5 pixels off-screen
- Separator at y=14 overlaps with the visible bottom portion of the title

### Fix

```cpp
u8g2.setFont(u8g2_font_6x13_tr);
u8g2.drawStr(2, 14, "PCGKolect RV1103");  // baseline at y=14
u8g2.drawHLine(0, 16, 128);                // separator at y=16
```

With baseline at y=14, the 13px-tall title spans y = 1 to y = 14 — fully visible on the 128×64 display.

### u8g2 Coordinate Reference

u8g2 uses **character-cell coordinates**, not pixel coordinates:

| Concept | Meaning |
|---------|---------|
| `drawStr(x, y, text)` | x = character column, y = text baseline row |
| Font height | Number of pixels from baseline to top of tallest character |
| `drawHLine(x, y, w)` | y = pixel row (not character row) |
| `clearBuffer()` | Clears the full RAM frame buffer (128×64 = 1024 bits) |
| `sendBuffer()` | Sends entire buffer to display in one I2C transaction |

### Final Layout

```
┌──────────────────────────────────┐  y=0
│ PCGKolect RV1103                 │  y=1..14 (title, baseline=14)
│──────────────────────────────────│  y=16 (separator)
│ Frames:      1234                │  y=21..28 (baseline=28)
│ CRC err:        0                │  y=31..38 (baseline=38)
│ Bytes:     234567                │  y=41..48 (baseline=48)
│ Buf:          0                  │  y=51..58 (baseline=58)
│                                   │  y=59..63 (empty)
└──────────────────────────────────┘  y=63
```

---

## Key Takeaways

1. **I2C repeated-start matters** — SSD1306 command/data switching requires a single I2C transaction with repeated start, not separate transactions
2. **u8g2 baseline coordinates** — `y` is the text baseline, not the top edge. Font height determines how far up the text extends
3. **Section-based garbage collection** — `-ffunction-sections -fdata-sections -Wl,--gc-sections` is essential when linking large libraries with many unused symbols
4. **Non-fatal display init** — UART/audio pipeline should continue if display fails; don't block boot on display
5. **u8g2-linux I2C implementation** — uses `I2C_RDWR` ioctl with `struct i2c_msg[]` array, sending control byte + command/data in one transaction

---

*Documented: 2026-10-13*
