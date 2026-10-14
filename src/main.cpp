#include "system_info.hpp"
#include "pcg_stream.hpp"
#include "display.hpp"
#include "seg_model.hpp"
#include "seg_quality.hpp"
#include "control_server.hpp"
#include "file_service.hpp"
#include "recording_store.hpp"
#include "session_engine.hpp"
#include "sha256.hpp"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <memory>
#include <thread>
#include <vector>

#include <sys/stat.h>
#include <unistd.h>

#ifndef RV_FW_VERSION
#define RV_FW_VERSION "dev"
#endif

static constexpr size_t FRAME_SAMPLES = 800;  // 100 ms @ 8 kHz

static std::atomic<bool> g_stop{false};
static void onSignal(int) { g_stop = true; }

int main(int argc, char** argv)
{
    setvbuf(stdout, nullptr, _IOLBF, 0);   // line-buffered log even when redirected
    printSystemInfo();
    std::cout << "RVFirmware " << RV_FW_VERSION << std::endl;

    // --- Display (SSD1306 on I2C3) ---
    Display::Config dispCfg;
    dispCfg.i2c_bus = 3;
    dispCfg.i2c_addr = 0x78;  // 0x3C << 1
    Display disp(dispCfg);
    if (!disp.init())
        std::cerr << "Display init failed (I2C bus " << dispCfg.i2c_bus << ")" << std::endl;

    // --- UART link to the ESP32 (audio + control). PCG_REPLAY=/path/pcm16_8k.raw replays a
    //     recording at real-time pace instead (same pipeline, no UART). ---
    PcgStream stream;
    PcgStream::Config streamCfg;
    const char* replayPath = std::getenv("PCG_REPLAY");
    FILE* replay = replayPath ? std::fopen(replayPath, "rb") : nullptr;
    if (replayPath && !replay) {
        std::cerr << "Cannot open replay file " << replayPath << std::endl;
        return 1;
    }
    if (replay) {
        std::cout << "REPLAY mode: " << replayPath << " (UART not used)" << std::endl;
    } else if (!stream.open(streamCfg)) {
        std::cerr << "Failed to open UART " << streamCfg.device << std::endl;
        return 1;
    } else {
        std::cout << "UART " << streamCfg.device << " @ " << streamCfg.baud << std::endl;
    }

    // --- Segmentation quality v2 (NPU) ---
    const char* modelPath = argc > 1 ? argv[1]
                          : (std::getenv("PCG_SEG_MODEL") ? std::getenv("PCG_SEG_MODEL") : "/root/segnet.rknn");
    SegModel segModel;
    std::unique_ptr<SegQuality> seg;
    SessionEngine::Info info;
    info.rv_fw = RV_FW_VERSION;
    info.model_version = "segnet-v2-w8a8";
    info.model_path = modelPath;
    if (segModel.load(modelPath)) {
        seg.reset(new SegQuality([&](const float* f, float* p) { return segModel.run(f, p); }));
        info.model_loaded = true;
        info.model_sha256 = Sha256::ofFile(modelPath);
        std::cout << "NPU model " << modelPath << " sha256 " << info.model_sha256.substr(0, 16) << "…" << std::endl;
    } else {
        std::cerr << "NPU model not loaded (" << modelPath << "): MODEL_ERROR" << std::endl;
    }

    // --- Recordings, session engine, control plane ---
    RecordingStore store(std::getenv("PCG_RECORD_DIR") ? std::getenv("PCG_RECORD_DIR") : "/root/recordings");
    if (!store.init()) std::cerr << "Recording store unavailable at " << store.root() << std::endl;
    SessionEngine engine(store, replay ? nullptr : &stream, info);
    engine.recoverPartials();
    engine.loadState();

    ControlServer ctrl(stream);
    FileService files(stream);
    files.setResolver([&store](const std::string& rid, const std::string& kind, std::string& path, std::string& sha) {
        return store.resolve(rid, kind, path, sha);
    });
    files.registerOps(ctrl);
    engine.registerOps(ctrl);
    engine.setEventSink(replay ? nullptr : &ctrl);
    ctrl.on("ping", [](ControlServer::Ctx&) {});
    ctrl.on("link_stats", [&stream, &ctrl](ControlServer::Ctx& c) {
        const auto s = stream.stats();
        cJSON_AddNumberToObject(c.rsp, "baud", s.baud);
        cJSON_AddNumberToObject(c.rsp, "frames_ok", (double)s.frames_ok);
        cJSON_AddNumberToObject(c.rsp, "crc_errors", (double)s.crc_errors);
        cJSON_AddNumberToObject(c.rsp, "malformed", (double)s.malformed);
        cJSON_AddNumberToObject(c.rsp, "seq_gaps", (double)s.seq_gaps);
        cJSON_AddNumberToObject(c.rsp, "discontinuity", (double)s.discontinuity);
        cJSON_AddNumberToObject(c.rsp, "overruns", (double)s.overruns);
        cJSON_AddNumberToObject(c.rsp, "bytes_read", (double)s.bytes_read);
        cJSON_AddNumberToObject(c.rsp, "msg_ok", (double)s.msg_ok);
        cJSON_AddNumberToObject(c.rsp, "msg_tx", (double)s.msg_tx);
        cJSON_AddNumberToObject(c.rsp, "req_dropped", (double)ctrl.dropped());
    }, false);
    ctrl.on("baud", [&stream](ControlServer::Ctx& c) {
        const int b = static_cast<int>(ControlServer::num(c.req, "baud", 0));
        if (b != 460800 && b != 921600 && b != 1500000 && b != 2000000 && b != 3000000 && b != 4000000)
            return ControlServer::reject(c, "INVALID", "baud");
        char* t = cJSON_PrintUnformatted(c.rsp);      // answer at the old rate, then switch
        stream.sendMsg(PcgStream::kRsp, t, std::strlen(t));
        cJSON_free(t);
        c.no_reply = true;
        stream.switchBaud(b);
    }, false);
    ctrl.on("flood", [&stream](ControlServer::Ctx& c) {   // link benchmark
        const uint32_t bytes = static_cast<uint32_t>(ControlServer::num(c.req, "bytes", 0));
        const uint32_t id = c.id;
        std::thread([&stream, bytes, id] {
            std::this_thread::sleep_for(std::chrono::milliseconds(50));
            std::vector<uint8_t> buf(4096);
            for (uint32_t off = 0; off < bytes; off += 4096) {
                const uint32_t n = std::min<uint32_t>(4096, bytes - off);
                for (uint32_t i = 0; i < n; i++) buf[i] = static_cast<uint8_t>((off + i) * 31u + 7u);
                stream.sendChunk(id, off, buf.data(), n);
            }
        }).detach();
    }, false);
    if (!replay) ctrl.start();

    // Local control FIFO for on-board testing (also in replay mode):
    //   echo '{"v":1,"id":1,"op":"status"}' > /tmp/pcg_ctrl.in ; cat /tmp/pcg_ctrl.out
    std::thread([&ctrl] {
        ::unlink("/tmp/pcg_ctrl.in");
        if (::mkfifo("/tmp/pcg_ctrl.in", 0600) != 0) return;
        for (;;) {
            FILE* in = std::fopen("/tmp/pcg_ctrl.in", "r");   // blocks until a writer opens it
            if (!in) return;
            char line[2048];
            while (std::fgets(line, sizeof(line), in)) {
                const std::string out = ctrl.handleLocal(line);
                FILE* o = std::fopen("/tmp/pcg_ctrl.out", "a");
                if (o) { std::fprintf(o, "%s\n", out.c_str()); std::fclose(o); }
            }
            std::fclose(in);
        }
    }).detach();

    std::signal(SIGINT, onSignal);
    std::signal(SIGTERM, onSignal);
    std::signal(SIGHUP, onSignal);

    // --- Main loop ---
    std::vector<int16_t> raw(FRAME_SAMPLES);
    std::vector<SegQuality::Beat> newBeats;
    static const std::vector<int16_t> zeros(FRAME_SAMPLES, 0);
    uint64_t expect_abs = 0;           // next absolute sample the pipeline expects
    uint64_t last_frames = 0;
    auto last_audio = std::chrono::steady_clock::now();
    auto lastReport = last_audio, lastDraw = last_audio;

    while (!g_stop) {
        size_t n;
        uint64_t abs = expect_abs;
        if (replay) {
            n = std::fread(raw.data(), sizeof(int16_t), FRAME_SAMPLES, replay);
            if (n == 0) {
                std::this_thread::sleep_for(std::chrono::seconds(3));
                break;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(100));   // real-time pace
        } else {
            n = stream.read(raw.data(), FRAME_SAMPLES, 200, &abs);
        }

        if (n > 0) {
            // Samples dropped by a receive overrun: keep the model on the same time axis.
            while (seg && abs > expect_abs) {
                const size_t z = static_cast<size_t>(std::min<uint64_t>(abs - expect_abs, FRAME_SAMPLES));
                newBeats.clear();
                seg->push(zeros.data(), z, newBeats);
                expect_abs += z;
            }
            expect_abs = abs + n;
            engine.onAudio(abs, raw.data(), n);
            if (seg) {
                newBeats.clear();
                if (seg->push(raw.data(), n, newBeats) > 0)
                    engine.onScored(static_cast<uint64_t>(seg->scoredSeconds() * 8000.0 + 0.5),
                                    seg->recentConf(), seg->recentNoise());
                for (const auto& b : newBeats) engine.onBeat(b);
            }
        }
        if (!replay)
            for (const auto& g : stream.takeGaps()) engine.onGap(g);
        engine.tick();

        const auto now = std::chrono::steady_clock::now();
        const auto ls = replay ? PcgStream::Stats() : stream.stats();
        if (replay || ls.frames_ok != last_frames) {
            last_frames = ls.frames_ok;
            last_audio = now;
        }
        if (now - lastReport >= std::chrono::seconds(10)) {
            lastReport = now;
            std::cout << "link frames=" << ls.frames_ok << " crc=" << ls.crc_errors << " gaps=" << ls.seq_gaps
                      << " ovr=" << ls.overruns << " msg=" << ls.msg_ok << "/" << ls.msg_tx << " baud=" << ls.baud
                      << (seg ? "" : " (no model)") << std::endl;
        }

        // --- OLED, 5 Hz ---
        if (disp.isInitialized() && now - lastDraw >= std::chrono::milliseconds(200)) {
            lastDraw = now;
            const auto v = engine.view();
            const bool link_ok = now - last_audio < std::chrono::seconds(2);
            const char* word;
            switch (v.state) {
                case SessionEngine::DONE:         word = "DONE"; break;
                case SessionEngine::INCOMPLETE:   word = "RETRY"; break;
                case SessionEngine::INTERRUPTED:
                case SessionEngine::STORAGE_FULL:
                case SessionEngine::MODEL_ERROR:  word = "ERR"; break;
                case SessionEngine::ARMED:
                case SessionEngine::RECORDING:
                case SessionEngine::FINALIZING:   word = "WIP"; break;
                default:                          word = "READY"; break;
            }
            const char* tag = !v.model_ok ? "NPU!" : !link_ok ? "LINK!" : v.storage_low ? "MEM!"
                            : v.state == SessionEngine::ARMED ? "ARM" : v.state == SessionEngine::RECORDING ? "REC" : "";
            disp.drawSession(word, link_ok ? v.q : -1, v.usable_s, v.req_s, v.progress, tag);
            disp.flush();
        }
    }

    ctrl.stop();
    stream.close();
    return 0;
}
