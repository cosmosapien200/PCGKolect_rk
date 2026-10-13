#include "system_info.hpp"
#include "pcg_stream.hpp"
#include "pcg_preprocessor.hpp"
#include "display.hpp"

#include <iostream>
#include <vector>
#include <chrono>
#include <cstdio>

static constexpr size_t FRAME_SAMPLES = 800;  // 100 ms @ 8 kHz

int main()
{
    printSystemInfo();

    // --- Display (SSD1306 on I2C3) ---
    Display::Config dispCfg;
    dispCfg.i2c_bus = 3;
    dispCfg.i2c_addr = 0x78;  // 0x3C << 1
    Display disp(dispCfg);
    if (!disp.init()) {
        std::cerr << "Display init failed (I2C bus " << dispCfg.i2c_bus
                  << ", addr 0x" << std::hex << static_cast<int>(dispCfg.i2c_addr)
                  << std::dec << ")" << std::endl;
        // Continue without display — UART still works
    } else {
        std::cout << "Display initialized on I2C bus " << dispCfg.i2c_bus
                  << std::endl;
    }

    // --- Configure & open UART ---
    PcgStream stream;
    PcgStream::Config streamCfg;
    streamCfg.device = "/dev/ttyS3";
    streamCfg.baud   = 460800;

    if (!stream.open(streamCfg)) {
        std::cerr << "Failed to open UART. Is /dev/ttyS3 available?" << std::endl;
        return 1;
    }
    std::cout << "UART connected: " << streamCfg.device
              << " @ " << streamCfg.baud << std::endl;

    // --- Preprocessor ---
    PcgPreprocessor::Config preCfg;
    preCfg.sample_rate = 8000;
    preCfg.low_hz      = 20.0f;
    preCfg.high_hz     = 500.0f;
    preCfg.normalize   = true;
    preCfg.target_rms  = 0.5f;
    PcgPreprocessor pre(preCfg);

    // --- Buffers ---
    std::vector<int16_t> raw(FRAME_SAMPLES);
    std::vector<float>   proc(FRAME_SAMPLES);

    // --- Main loop ---
    std::cout << "Listening on " << streamCfg.device
              << " (Ctrl+C to stop)..." << std::endl;

    auto lastReport = std::chrono::steady_clock::now();
    auto lastFlush  = std::chrono::steady_clock::now();
    int  frameCount = 0;
    int  crcErrors  = 0;
    int  prevFrames = 0;
    int  prevCrcErr = 0;

    while (true) {
        size_t n = stream.read(raw.data(), FRAME_SAMPLES, 200);
        if (n > 0) {
            pre.process(raw.data(), proc.data(), n);
            frameCount++;
        }

        auto now = std::chrono::steady_clock::now();

        // --- Console health report every 2 seconds ---
        if (now - lastReport >= std::chrono::seconds(2)) {
            lastReport = now;
            auto s = stream.stats();
            std::cout << "frames=" << s.frames_ok
                      << " crc_err=" << s.crc_errors
                      << " bytes=" << s.bytes_read
                      << " avail=" << stream.available()
                      << std::endl;
        }

        // --- Display flush every 200ms ---
        if (now - lastFlush >= std::chrono::milliseconds(200)) {
            lastFlush = now;
            if (disp.isInitialized()) {
                auto s = stream.stats();
                int crcRate = (s.crc_errors - prevCrcErr) / 2;
                disp.drawStatus(s.frames_ok, s.crc_errors,
                                s.bytes_read, stream.available(), crcRate);
                disp.flush();
                prevFrames = s.frames_ok;
                prevCrcErr = s.crc_errors;
            }
        }
    }

    stream.close();
    return 0;
}
