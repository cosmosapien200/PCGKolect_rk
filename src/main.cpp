#include "system_info.hpp"
#include "pcg_stream.hpp"
#include "pcg_preprocessor.hpp"
#include "display.hpp"
#include "signal_quality.hpp"
#include "seg_model.hpp"
#include "seg_quality.hpp"
#include "session_recorder.hpp"

#include <iostream>
#include <vector>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <memory>
#include <algorithm>
#include <thread>
#include <atomic>
#include <csignal>

static constexpr size_t FRAME_SAMPLES = 800;  // 100 ms @ 8 kHz

static std::atomic<bool> g_stop{false};
static void onSignal(int) { g_stop = true; }

int main(int argc, char** argv)
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

    // Test mode: PCG_REPLAY=/path/pcm16_8k.raw plays a recording at real-time pace instead of UART
    // (exercises the exact same quality pipeline and OLED screens).
    const char* replayPath = std::getenv("PCG_REPLAY");
    FILE* replay = replayPath ? std::fopen(replayPath, "rb") : nullptr;
    if (replayPath && !replay) {
        std::cerr << "Cannot open replay file " << replayPath << std::endl;
        return 1;
    }
    if (replay) {
        std::cout << "REPLAY mode: " << replayPath << " (UART not used)" << std::endl;
    } else if (!stream.open(streamCfg)) {
        std::cerr << "Failed to open UART. Is /dev/ttyS3 available?" << std::endl;
        return 1;
    }
    if (!replay)
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

    // --- Signal quality (scores raw PCM once per second) ---
    SignalQuality quality;
    QualityTracker tracker;
    std::vector<SignalQuality::Result> qres;
    SignalQuality::Result lastQ;
    bool haveQ = false;

    // --- Segmentation quality v2 (NPU). Falls back to v1-only if the model is missing ---
    const char* modelPath = argc > 1 ? argv[1]
                          : (std::getenv("PCG_SEG_MODEL") ? std::getenv("PCG_SEG_MODEL") : "/root/segnet.rknn");
    SegModel segModel;
    std::unique_ptr<SegQuality> seg;
    BeatTracker beatTracker;
    std::vector<SegQuality::Beat> newBeats;
    SegQuality::Beat lastBeat;
    double lastBeatWall = -1e9;
    if (segModel.load(modelPath)) {
        seg.reset(new SegQuality([&](const float* f, float* p) { return segModel.run(f, p); }));
        std::cout << "NPU segmentation model loaded: " << modelPath << std::endl;
    } else {
        std::cout << "NPU model not loaded (" << modelPath << "), using v1 quality only" << std::endl;
    }
    const auto tStart = std::chrono::steady_clock::now();

    // --- Buffers ---
    std::vector<int16_t> raw(FRAME_SAMPLES);
    std::vector<float>   proc(FRAME_SAMPLES);

    // --- Session recording (audio + beats + per-second quality) for review / fine-tuning ---
    // PCG_RECORD_DIR overrides /root/recordings; PCG_NO_RECORD=1 disables. Off in replay mode.
    SessionRecorder rec;
    if (!replay && !std::getenv("PCG_NO_RECORD")) {
        const char* dir = std::getenv("PCG_RECORD_DIR") ? std::getenv("PCG_RECORD_DIR") : "/root/recordings";
        if (rec.open(dir)) std::cout << "Recording session to " << rec.name() << ".wav" << std::endl;
        else std::cerr << "Session recording disabled (cannot write " << dir << ")" << std::endl;
    }
    std::signal(SIGINT, onSignal);
    std::signal(SIGTERM, onSignal);
    std::signal(SIGHUP, onSignal);

    // --- Main loop ---
    std::cout << "Listening on " << streamCfg.device
              << " (Ctrl+C to stop)..." << std::endl;

    auto lastReport = std::chrono::steady_clock::now();
    auto lastFlush  = std::chrono::steady_clock::now();
    int  frameCount = 0;
    int  crcErrors  = 0;
    int  prevFrames = 0;
    int  prevCrcErr = 0;

    while (!g_stop) {
        size_t n;
        if (replay) {
            n = std::fread(raw.data(), sizeof(int16_t), FRAME_SAMPLES, replay);
            if (n == 0) {
                if (seg) {
                    const auto& t = beatTracker.summary();
                    std::printf("REPLAY done: usable beats=%d longest run=%d hr=%.0f usable=%.1fs of %.1fs\n",
                                t.usable_beats, t.longest_run, t.hr_bpm, t.usable_s, seg->scoredSeconds());
                }
                std::this_thread::sleep_for(std::chrono::seconds(3));   // leave the final screen up
                break;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(100));   // 800 samples = 100 ms
        } else {
            n = stream.read(raw.data(), FRAME_SAMPLES, 200);
        }
        if (n > 0) {
            pre.process(raw.data(), proc.data(), n);
            frameCount++;
            rec.writeAudio(raw.data(), n);

            qres.clear();
            quality.push(raw.data(), n, qres);
            for (const auto& r : qres) {
                tracker.add(r);
                lastQ = r;
                haveQ = true;
                const auto& t = tracker.summary();
                std::printf("quality t=%.0f-%.0fs q=%.2f %s/%s level=%.1fdBFS hr=%.0f | "
                            "usable=%.0f/%.0fs beats=%.0f\n",
                            r.t_start_s, r.t_end_s, r.q,
                            SignalQuality::verdictName(r.verdict),
                            SignalQuality::reasonName(r.reason),
                            r.level_dbfs, r.hr_bpm, t.good_s, t.scored_s, t.usable_beats);
                std::fflush(stdout);
                rec.logQuality(r.t_start_s, r.t_end_s, r.q, SignalQuality::verdictName(r.verdict),
                               SignalQuality::reasonName(r.reason), r.level_dbfs,
                               seg ? seg->recentNoise() : -1, seg ? seg->recentConf() : -1);
            }

            if (seg) {
                newBeats.clear();
                seg->push(raw.data(), n, newBeats);
                // Deterministic gates from v1 (clipping, no contact, too loud) veto beats.
                const bool gated = haveQ && (lastQ.reason == SignalQuality::CLIPPING ||
                                             lastQ.reason == SignalQuality::NO_CONTACT ||
                                             lastQ.reason == SignalQuality::TOO_LOUD);
                for (auto& b : newBeats) {
                    if (gated) b.usable = false;
                    beatTracker.add(b);
                    rec.logBeat(b.t_start_s, b.t_end_s, b.conf, b.pnoise, b.usable, gated);
                    lastBeat = b;
                    lastBeatWall = std::chrono::duration<double>(std::chrono::steady_clock::now() - tStart).count();
                    const auto& t = beatTracker.summary();
                    std::printf("beat t=%.2f-%.2fs dur=%.2fs conf=%.2f pnoise=%.2f %s%s | usable=%d run=%d "
                                "coverage=%.0f%% hr=%.0f npu=%.1fms\n",
                                b.t_start_s, b.t_end_s, b.dur_s, b.conf, b.pnoise,
                                b.usable ? "USABLE" : "REJECT", gated ? "(gate)" : "",
                                t.usable_beats, t.run, 100.0 * t.usable_s / std::max(1.0, seg->scoredSeconds()),
                                t.hr_bpm, seg->lastInferMs());
                }
                std::fflush(stdout);
            }
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
                if (seg && seg->lastInferMs() > 0) {
                    const auto& t = beatTracker.summary();
                    const double nowS = std::chrono::duration<double>(now - tStart).count();
                    const char* status;
                    if (haveQ && lastQ.reason != SignalQuality::OK && lastQ.reason != SignalQuality::NOISY)
                        status = SignalQuality::reasonName(lastQ.reason);
                    else if (seg->recentNoise() > 0.5)
                        status = "NOISY";
                    else if (nowS - lastBeatWall < 3.0 && lastBeat.usable)
                        status = "GOOD";
                    else
                        status = "SEARCHING";
                    const double q = seg->recentConf() * (1.0 - seg->recentNoise());
                    disp.drawBeats(status, q, t.hr_bpm,
                                   t.usable_beats, t.longest_run, t.usable_s, seg->scoredSeconds(), s.crc_errors);
                    static int oledLog = 0;
                    if (replay && ++oledLog % 10 == 0)   // every 2 s: echo what the OLED shows
                        std::printf("oled: %-10s Q %3.0f%% | HR %3.0f | run %d | usable beats %d | cover %.0f%% (%.0f/%.0fs)\n",
                                    status, q * 100, t.hr_bpm, t.longest_run, t.usable_beats,
                                    100.0 * t.usable_s / std::max(1.0, seg->scoredSeconds()),
                                    t.usable_s, seg->scoredSeconds());
                } else if (haveQ) {
                    const auto& t = tracker.summary();
                    disp.drawQuality(lastQ.q, SignalQuality::verdictName(lastQ.verdict),
                                     SignalQuality::reasonName(lastQ.reason), lastQ.hr_bpm,
                                     t.good_s, t.scored_s, t.usable_beats, s.crc_errors);
                } else {
                    disp.drawStatus(s.frames_ok, s.crc_errors,
                                    s.bytes_read, stream.available(), crcRate);
                }
                disp.flush();
                prevFrames = s.frames_ok;
                prevCrcErr = s.crc_errors;
            }
        }
    }

    rec.close();
    if (rec.isOpen() == false && !rec.name().empty())
        std::cout << "\nSession saved: " << rec.name() << ".wav (+ _beats.csv, _quality.csv)" << std::endl;
    if (seg) {
        const auto& t = beatTracker.summary();
        std::printf("Session summary: usable beats=%d longest run=%d hr=%.0f usable=%.1fs of %.1fs\n",
                    t.usable_beats, t.longest_run, t.hr_bpm, t.usable_s, seg->scoredSeconds());
    }
    stream.close();
    return 0;
}
