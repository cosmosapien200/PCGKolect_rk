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

    // --- Title (6x13 font, 13px tall; baseline at y=14 puts text in y=1..14) ---
    u8g2.setFont(u8g2_font_6x13_tr);
    u8g2.drawStr(2, 14, "PCGKolect RV1103");

    // --- Separator line ---
    u8g2.drawHLine(0, 16, 128);

    // --- Stat rows (5x7 font, 7px tall) ---
    u8g2.setFont(u8g2_font_5x7_tf);
    char buf[64];

    snprintf(buf, sizeof(buf), "Frames: %6d", frames_ok);
    u8g2.drawStr(2, 28, buf);

    snprintf(buf, sizeof(buf), "CRC err: %5d", crc_errors);
    u8g2.drawStr(2, 38, buf);

    snprintf(buf, sizeof(buf), "Bytes: %7d", bytes_read);
    u8g2.drawStr(2, 48, buf);

    snprintf(buf, sizeof(buf), "Buf: %5d", buffer_available);
    u8g2.drawStr(2, 58, buf);

    // --- CRC rate indicator (right side, aligned with CRC err row) ---
    if (crc_rate > 5) {
        u8g2.drawStr(90, 38, "!CRC!");
    } else if (crc_rate > 0) {
        u8g2.drawStr(90, 38, "CRC");
    }

    // --- Uptime / heartbeat dot ---
    static int blink = 0;
    blink++;
    if (blink % 30 < 15) {
        u8g2.drawPixel(124, 6);
    }
}

void Display::drawQuality(double q, const char* verdict, const char* reason, double hr_bpm,
                          double good_s, double scored_s, double usable_beats, int crc_errors)
{
    u8g2.clearBuffer();
    char buf[32];

    // --- Verdict (large) + score ---
    u8g2.setFont(u8g2_font_6x13_tr);
    u8g2.drawStr(2, 12, verdict);
    snprintf(buf, sizeof(buf), "Q %3d%%", static_cast<int>(q * 100 + 0.5));
    u8g2.drawStr(86, 12, buf);

    // --- Quality bar ---
    u8g2.drawFrame(0, 16, 128, 7);
    u8g2.drawBox(1, 17, static_cast<int>(std::min(1.0, std::max(0.0, q)) * 126), 5);

    u8g2.setFont(u8g2_font_5x7_tf);
    if (hr_bpm > 0)
        snprintf(buf, sizeof(buf), "HR ~%3d  %s", static_cast<int>(hr_bpm + 0.5), reason);
    else
        snprintf(buf, sizeof(buf), "HR  --   %s", reason);
    u8g2.drawStr(2, 33, buf);

    snprintf(buf, sizeof(buf), "Usable %5.0fs / %5.0fs", good_s, scored_s);
    u8g2.drawStr(2, 43, buf);

    snprintf(buf, sizeof(buf), "Beats %4d  USR %3d%%", static_cast<int>(usable_beats),
             scored_s > 0 ? static_cast<int>(100 * good_s / scored_s + 0.5) : 0);
    u8g2.drawStr(2, 53, buf);

    snprintf(buf, sizeof(buf), "CRC err %d", crc_errors);
    u8g2.drawStr(2, 63, buf);
}

void Display::drawBeats(const char* status, double q, double hr_bpm, int usable_beats,
                        int longest_run, double usable_s, double scored_s, int crc_errors)
{
    u8g2.clearBuffer();
    char buf[32];

    u8g2.setFont(u8g2_font_6x13_tr);
    u8g2.drawStr(2, 12, status);
    snprintf(buf, sizeof(buf), "Q %3d%%", static_cast<int>(q * 100 + 0.5));
    u8g2.drawStr(86, 12, buf);

    u8g2.drawFrame(0, 16, 128, 7);
    u8g2.drawBox(1, 17, static_cast<int>(std::min(1.0, std::max(0.0, q)) * 126), 5);

    u8g2.setFont(u8g2_font_5x7_tf);
    if (hr_bpm > 0) snprintf(buf, sizeof(buf), "HR %3d  Run %3d beats", static_cast<int>(hr_bpm + 0.5), longest_run);
    else            snprintf(buf, sizeof(buf), "HR  --  Run %3d beats", longest_run);
    u8g2.drawStr(2, 33, buf);

    snprintf(buf, sizeof(buf), "Usable beats %5d", usable_beats);
    u8g2.drawStr(2, 43, buf);

    snprintf(buf, sizeof(buf), "Cover %3d%%  %4.0f/%4.0fs",
             scored_s > 0 ? static_cast<int>(100 * usable_s / scored_s + 0.5) : 0, usable_s, scored_s);
    u8g2.drawStr(2, 53, buf);

    snprintf(buf, sizeof(buf), "CRC err %d   NPU", crc_errors);
    u8g2.drawStr(2, 63, buf);
}

void Display::drawSession(const char* word, int q, double usable_s, double req_s, double progress,
                          const char* tag)
{
    u8g2.clearBuffer();
    char buf[32];

    // Left: vertical quality bar, 8 px wide, filled from the bottom.
    const int bx = 0, by = 0, bw = 8, bh = 64;
    u8g2.drawFrame(bx, by, bw, bh);
    if (q >= 0) {
        const int fill = (std::min(100, q) * (bh - 2)) / 100;
        u8g2.drawBox(bx + 1, by + bh - 1 - fill, bw - 2, fill);
    } else {
        for (int y = by + 2; y < by + bh - 1; y += 4)      // hatched = no measurement
            u8g2.drawLine(bx + 1, y + 2, bx + bw - 2, y);
    }

    // Tiny indicator, top right.
    if (tag && *tag) {
        u8g2.setFont(u8g2_font_5x7_tf);
        u8g2.drawStr(128 - u8g2.getStrWidth(tag), 7, tag);
    }

    // Status word, large and centred in the area right of the bar.
    u8g2.setFont(u8g2_font_logisoso24_tr);
    const int area_x = 12, area_w = 116;
    int w = u8g2.getStrWidth(word);
    u8g2.drawStr(area_x + std::max(0, (area_w - w) / 2), 34, word);

    // Collected usable time / needed time.
    u8g2.setFont(u8g2_font_6x13_tr);
    if (req_s > 0) snprintf(buf, sizeof(buf), "%.1f/%.0fs", usable_s, req_s);
    else           snprintf(buf, sizeof(buf), "%.1fs", usable_s);
    w = u8g2.getStrWidth(buf);
    u8g2.drawStr(area_x + std::max(0, (area_w - w) / 2), 50, buf);

    // Bottom: progress toward the usable-time target.
    const int px = 12, py = 56, pw = 116, ph = 8;
    u8g2.drawFrame(px, py, pw, ph);
    const int pf = static_cast<int>(std::min(1.0, std::max(0.0, progress)) * (pw - 2));
    if (pf > 0) u8g2.drawBox(px + 1, py + 1, pf, ph - 2);
}

void Display::flush()
{
    u8g2.sendBuffer();
}

void Display::clear()
{
    u8g2.clearDisplay();
}
