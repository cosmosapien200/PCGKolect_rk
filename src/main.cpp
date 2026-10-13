#include "system_info.hpp"
#include "pcg_stream.hpp"
#include "pcg_preprocessor.hpp"

#include <iostream>
#include <vector>
#include <chrono>

static constexpr size_t FRAME_SAMPLES = 800;  // 100 ms @ 8 kHz

int main()
{
    printSystemInfo();

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
    while (true) {
        size_t n = stream.read(raw.data(), FRAME_SAMPLES, 200);
        if (n > 0) {
            pre.process(raw.data(), proc.data(), n);

            // TODO: convert proc[] to spectrogram, feed to AI model
            // e.g.: auto spec = stft(proc.data(), n);
            //        model.predict(spec);
        }

        // Health report every 2 seconds.
        auto now = std::chrono::steady_clock::now();
        if (now - lastReport >= std::chrono::seconds(2)) {
            lastReport = now;
            auto s = stream.stats();
            std::cout << "frames=" << s.frames_ok
                      << " crc_err=" << s.crc_errors
                      << " bytes=" << s.bytes_read
                      << " avail=" << stream.available()
                      << std::endl;
        }
    }

    stream.close();
    return 0;
}
