#include "display.hpp"

#include <U8g2lib.h>

#include <cstdio>
#include <cstring>
#include <algorithm>

// SSD1306 128×64, full-frame buffer, hardware I2C
static U8G2_SSD1306_128X64_NONAME_F_HW_I2C u8g2(
    U8G2_R0,                // orientation
    U8X8_PIN_NONE           // reset pin (shares board reset)
);

static Display::Config g_config;

Display::Display(const Config& cfg)
    : config_(cfg)
{
    g_config = cfg;
}

bool Display::init()
{
    if (initialized_)
        return true;

    u8g2.initI2cHw(config_.i2c_bus);
    u8g2.setI2CAddress(config_.i2c_addr);
    u8g2.begin();
    clear();
    flush();

    initialized_ = true;
    return true;
}

void Display::drawStatus(int frames_ok, int crc_errors, int bytes_read,
                         int buffer_available, int crc_rate)
{
    u8g2.clearBuffer();

    // --- Title bar ---
    u8g2.setFont(u8g2_font_6x13_tr);
    u8g2.drawStr(2, 8, "PCGKolect RV1103");

    // --- Separator line ---
    u8g2.drawHLine(0, 14, 128);

    // --- Row 1: Frame rate / CRC ---
    u8g2.setFont(u8g2_font_5x7_tf);
    char buf[64];
    snprintf(buf, sizeof(buf), "Frames: %6d", frames_ok);
    u8g2.drawStr(2, 26, buf);

    snprintf(buf, sizeof(buf), "CRC err: %5d", crc_errors);
    u8g2.drawStr(2, 36, buf);

    // --- Row 2: Bytes / Buffer ---
    snprintf(buf, sizeof(buf), "Bytes: %7d", bytes_read);
    u8g2.drawStr(2, 46, buf);

    snprintf(buf, sizeof(buf), "Buf: %5d", buffer_available);
    u8g2.drawStr(2, 56, buf);

    // --- CRC rate indicator (right side) ---
    if (crc_rate > 5) {
        u8g2.drawStr(90, 36, "!CRC!");
    } else if (crc_rate > 0) {
        u8g2.drawStr(90, 36, "CRC");
    }

    // --- Uptime / heartbeat dot ---
    static int blink = 0;
    blink++;
    if (blink % 30 < 15) {
        u8g2.drawPixel(124, 6);
    }
}

void Display::flush()
{
    u8g2.sendBuffer();
}

void Display::clear()
{
    u8g2.clearDisplay();
}
