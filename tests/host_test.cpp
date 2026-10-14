// Host-side tests for the link parser and the session engine (no board needed).
//
//   make -C tests host-test        (builds with the host g++ and runs)
//
// 1. PcgStream parses audio + message frames from a pseudo-terminal: split writes,
//    corrupt CRC, garbage, SEQ gaps (zero-filled and reported), message dispatch.
// 2. SessionEngine driven by synthetic beats: dataset validation, prepare guards,
//    auto-start after contiguous usable beats, completion, files + metadata on disk,
//    ack/delete guards, crash recovery.
#include <fcntl.h>
#include <pty.h>
#include <sys/stat.h>
#include <termios.h>
#include <unistd.h>

#include <chrono>
#include <cstdio>
#include <cstring>
#include <string>
#include <thread>
#include <vector>

#include "control_server.hpp"
#include "pcg_stream.hpp"
#include "recording_store.hpp"
#include "session_engine.hpp"

static int g_fail = 0;
#define CHECK(c)                                                              \
    do {                                                                      \
        if (!(c)) {                                                           \
            std::printf("FAIL %s:%d  %s\n", __FILE__, __LINE__, #c);          \
            g_fail++;                                                         \
        }                                                                     \
    } while (0)

static std::vector<uint8_t> audioFrame(uint8_t seq, int16_t value, bool corrupt = false) {
    std::vector<uint8_t> f = {0xA5, 0x5A, 0x40, 0x06, seq};   // LEN 1600
    for (int i = 0; i < 800; i++) {
        f.push_back(static_cast<uint8_t>(value & 0xFF));
        f.push_back(static_cast<uint8_t>(value >> 8));
    }
    uint16_t crc = PcgStream::crc16(f.data() + 2, 3 + 1600);
    if (corrupt) crc ^= 0x1234;
    f.push_back(crc & 0xFF);
    f.push_back(crc >> 8);
    return f;
}

static std::vector<uint8_t> msgFrame(uint8_t type, const std::string& s) {
    std::vector<uint8_t> f = {0xA5, 0x5C, static_cast<uint8_t>(s.size() & 0xFF), static_cast<uint8_t>(s.size() >> 8), type, 0};
    f.insert(f.end(), s.begin(), s.end());
    uint16_t crc = PcgStream::crc16(f.data() + 2, 4 + s.size());
    f.push_back(crc & 0xFF);
    f.push_back(crc >> 8);
    return f;
}

static void testParser() {
    std::printf("== link parser\n");
    CHECK(PcgStream::crc16(reinterpret_cast<const uint8_t*>("123456789"), 9) == 0x29B1);
    auto m = msgFrame(PcgStream::kReq, "{}");
    const uint8_t vec[] = {0xA5, 0x5C, 0x02, 0x00, 0x01, 0x00, 0x7B, 0x7D, 0x7D, 0x88};
    CHECK(m.size() == sizeof(vec) && std::memcmp(m.data(), vec, sizeof(vec)) == 0);   // PROTOCOL.md §1.4

    int master, slave;
    char name[128];
    CHECK(openpty(&master, &slave, name, nullptr, nullptr) == 0);
    PcgStream s;
    PcgStream::Config cfg;
    cfg.device = name;
    std::vector<std::string> msgs;
    s.setMessageHandler([&](uint8_t t, const uint8_t* d, size_t n) {
        if (t == PcgStream::kReq) msgs.emplace_back(reinterpret_cast<const char*>(d), n);
    });
    CHECK(s.open(cfg));
    auto put = [&](const std::vector<uint8_t>& b) { CHECK(::write(master, b.data(), b.size()) == static_cast<ssize_t>(b.size())); };

    put({0x00, 0x13, 0xA5, 0x11});                    // garbage, including a false sync
    auto f0 = audioFrame(0, 100);
    put(std::vector<uint8_t>(f0.begin(), f0.begin() + 700));   // split write
    put(std::vector<uint8_t>(f0.begin() + 700, f0.end()));
    put(audioFrame(1, 200));
    put(audioFrame(2, 300, true));                    // CRC error -> dropped
    put(msgFrame(PcgStream::kReq, R"({"op":"ping"})"));
    put(audioFrame(4, 400));                          // SEQ 2,3 missing -> 2 frames zero-filled
    std::this_thread::sleep_for(std::chrono::milliseconds(400));

    std::vector<int16_t> out(8000);
    uint64_t start = 99;
    const size_t n = s.read(out.data(), out.size(), 300, &start);
    const auto st = s.stats();
    CHECK(start == 0);
    CHECK(n == 5 * 800);                              // 3 good frames + 2 filled
    CHECK(out[0] == 100 && out[800] == 200);
    CHECK(out[1600] == 0 && out[2399] == 0 && out[3199] == 0);   // the gap is zeros
    CHECK(out[3200] == 400);
    CHECK(st.frames_ok == 3);
    CHECK(st.crc_errors == 1);
    CHECK(st.seq_gaps == 2);
    const auto gaps = s.takeGaps();
    CHECK(gaps.size() == 1 && gaps[0].at_sample == 1600 && gaps[0].samples == 1600);
    CHECK(msgs.size() == 1 && msgs[0] == R"({"op":"ping"})");

    put(audioFrame(200, 1));                          // jump of 195 frames: discontinuity, not filled
    std::this_thread::sleep_for(std::chrono::milliseconds(200));
    CHECK(s.stats().discontinuity == 1);
    s.close();
    ::close(master);
    ::close(slave);
}

static std::string call(ControlServer& c, const std::string& json) { return c.handleLocal(json); }

static bool has(const std::string& s, const char* sub) { return s.find(sub) != std::string::npos; }

static SegQuality::Beat beat(long start_frame, long len, double conf) {
    SegQuality::Beat b;
    b.start_frame = start_frame;
    b.end_frame = start_frame + len;
    b.conf = conf;
    b.ok_dur = true;
    b.ok_rhythm = true;
    return b;
}

static void testSession() {
    std::printf("== session engine\n");
    char tmpl[] = "/tmp/pcgk_test_XXXXXX";
    const std::string root = std::string(mkdtemp(tmpl)) + "/rec";
    RecordingStore store(root);
    CHECK(store.init());
    PcgStream link;   // not opened: events are dropped, like replay mode
    SessionEngine::Info info;
    info.rv_fw = "test";
    info.model_version = "segnet-test";
    info.model_loaded = true;
    SessionEngine eng(store, nullptr, info);
    ControlServer ctrl(link);
    eng.registerOps(ctrl);

    const std::string ds = "11111111-2222-3333-4444-555555555555";
    const std::string rid = "aaaaaaaa-0000-4000-8000-000000000001";
    const std::string subj = "bbbbbbbb-0000-4000-8000-000000000002";

    // invalid settings are rejected with the field name
    auto r = call(ctrl, R"({"v":1,"id":1,"op":"set_dataset","dataset_id":")" + ds + R"(","name":"T","rev":1,"settings":{"target_usable_s":0}})");
    CHECK(has(r, "INVALID") && has(r, "target_usable_s"));
    r = call(ctrl, R"({"v":1,"id":2,"op":"set_dataset","dataset_id":")" + ds +
                       R"(","name":"T","rev":1,"utc_ms":1792000000000,"settings":{"target_usable_s":6,"autostart_s":2,"conf_threshold":0.8}})");
    CHECK(has(r, R"("st":"OK")"));
    const std::string prep = R"({"v":1,"id":3,"op":"prepare","dataset_id":")" + ds + R"(","subject_id":")" + subj +
                             R"(","recording_id":")" + rid + R"(","rev":1,"take":1})";
    CHECK(has(call(ctrl, R"({"v":1,"id":4,"op":"prepare","dataset_id":")" + ds + R"(","subject_id":")" + subj +
                         R"(","recording_id":")" + rid + R"(","rev":7,"take":1})"),
              "SETTINGS_MISMATCH"));
    CHECK(has(call(ctrl, prep), "ARMED"));

    // audio: 30 s of a ramp; beats of 1 s (50 frames) every second
    std::vector<int16_t> pcm(800);
    uint64_t abs = 0;
    long frame = 0;
    for (int t = 0; t < 300 && eng.view().state != SessionEngine::DONE; t++) {   // 100 ms steps
        for (auto& v : pcm) v = static_cast<int16_t>((abs++ % 200) * 10);
        eng.onAudio(abs - 800, pcm.data(), pcm.size());
        if (t % 10 == 9) {
            const bool low = (t / 10) == 4;   // one low-confidence beat breaks the run
            eng.onBeat(beat(frame, 50, low ? 0.6 : 0.9));
            frame += 50;
            eng.onScored(static_cast<uint64_t>(frame) * 160, 0.9, 0.05);
        }
        eng.tick();
    }
    const auto v = eng.view();
    CHECK(v.state == SessionEngine::DONE);
    CHECK(v.usable_s >= 6.0 && v.usable_s < 7.5);
    CHECK(has(call(ctrl, prep), "DUPLICATE_RECORDING"));

    // files and metadata
    const std::string dir = root + "/" + rid;
    struct stat stbuf;
    for (const char* f : {"audio.wav", "beats.csv", "quality.csv", "meta.json", "files.json", "transfer.json"})
        CHECK(::stat((dir + "/" + f).c_str(), &stbuf) == 0);
    std::string meta;
    fsutil::readFile(dir + "/meta.json", meta);
    CHECK(has(meta, R"("completion_state":	"complete")"));
    CHECK(has(meta, R"("snr_db":	null)"));
    CHECK(has(meta, "2026-"));   // wall clock from utc_ms

    // ack / delete guards
    std::string path, sha;
    CHECK(store.resolve(rid, "audio", path, sha));
    CHECK(has(call(ctrl, R"({"v":1,"id":10,"op":"delete","recording_id":")" + rid + R"(","sha256":")" + sha + R"("})"), "NOT_VERIFIED"));
    CHECK(has(call(ctrl, R"({"v":1,"id":11,"op":"ack","recording_id":")" + rid + R"(","sha256":"00"})"), "HASH_MISMATCH"));
    CHECK(has(call(ctrl, R"({"v":1,"id":12,"op":"ack","recording_id":")" + rid + R"(","sha256":")" + sha + R"("})"), "verified"));
    CHECK(has(call(ctrl, R"({"v":1,"id":13,"op":"clear","force":true})"), "FORCE_DISABLED"));
    CHECK(has(call(ctrl, R"({"v":1,"id":14,"op":"delete","recording_id":")" + rid + R"(","sha256":")" + sha + R"("})"), R"("deleted":true)"));
    CHECK(::stat(dir.c_str(), &stbuf) != 0);

    // crash recovery: a partial session becomes an interrupted recording
    const std::string rid2 = "aaaaaaaa-0000-4000-8000-000000000003";
    const std::string part = store.partialDir(rid2);
    fsutil::mkdirs(part);
    fsutil::writeFileAtomic(part + "/session.json", R"({"recording_id":")" + rid2 + R"(","dataset_id":")" + ds +
                                                        R"(","subject_id":")" + subj + R"(","take_index":2,"policy":{}})");
    FILE* w = std::fopen((part + "/audio.wav").c_str(), "wb");
    std::vector<uint8_t> junk(44 + 16000, 0);
    std::fwrite(junk.data(), 1, junk.size(), w);
    std::fclose(w);
    fsutil::writeFileAtomic(part + "/beats.csv", "start_sample,end_sample,conf,pnoise,usable,reason\n0,8000,0.9,0.1,1,OK\n");
    SessionEngine eng2(store, nullptr, info);
    eng2.recoverPartials();
    std::string meta2;
    CHECK(fsutil::readFile(root + "/" + rid2 + "/meta.json", meta2));
    CHECK(has(meta2, "interrupted"));
    CHECK(has(meta2, R"("usable_beats":	1)"));
    fsutil::removeTree(root.substr(0, root.size() - 4));
}

int main() {
    testParser();
    testSession();
    std::printf(g_fail ? "%d CHECK(S) FAILED\n" : "ALL HOST TESTS PASSED\n", g_fail);
    return g_fail ? 1 : 0;
}
