#ifndef DISPLAY_HPP
#define DISPLAY_HPP

#include <cstdint>

class Display {
public:
    struct Config {
        int  i2c_bus      = 3;   // I2C bus number (I2C3 on Luckfox Pico)
        uint8_t i2c_addr  = 0x78; // SSD1306 address << 1 (0x3C << 1)
        int  reset_pin    = -1;   // U8X8_PIN_NONE = -1 (shares board reset)
    };

    Display() = default;
    explicit Display(const Config& cfg);

    /// Initialize I2C and the SSD1306 display. Returns true on success.
    bool init();

    /// Draw the current PCG stream status and return.
    void drawStatus(int frames_ok, int crc_errors, int bytes_read,
                    int buffer_available, int crc_rate);

    /// Flush the frame buffer to the display.
    void flush();

    /// Clear the display.
    void clear();

    /// Check if display was initialized successfully.
    bool isInitialized() const { return initialized_; }

private:
    Config config_;
    bool   initialized_ = false;
};

#endif // DISPLAY_HPP
