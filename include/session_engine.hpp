#pragma once

#include <chrono>
#include <cstdint>
#include <cstdio>
#include <deque>
#include <mutex>
#include <string>
#include <vector>

#include "cJSON.h"
#include "control_server.hpp"
#include "pcg_stream.hpp"
#include "policy.hpp"
#include "recording_store.hpp"
#include "seg_quality.hpp"

// Authoritative recording session state machine (spec 03 §4–§7, §12).
//
//   IDLE -> CONFIGURED -(prepare)-> ARMED -(2 s usable / force_start)-> RECORDING
//        -> FINALIZING -> DONE | INCOMPLETE ;  ARMED -(cancel)-> CANCELLED
//   crash/reboot during RECORDING -> INTERRUPTED (recovered at next start)
//
// Time axis: absolute stream sample index (8 kHz) from PcgStream; the recording
// covers [rec_start_abs_, written_abs_). A beat is usable when the v2 decoder
// emitted it, conf >= policy.conf_threshold, its duration is valid and (if
// enabled) its rhythm check passed. Usable time is the union of usable beats.
//
// The audio thread calls onAudio/onBeat/onScored/onGap/tick; control ops arrive
// on the ControlServer worker. One mutex guards everything.
class SessionEngine {
public:
    enum State { IDLE, CONFIGURED, ARMED, RECORDING, FINALIZING, DONE, INCOMPLETE, CANCELLED,
                 INTERRUPTED, STORAGE_FULL, MODEL_ERROR };
    static const char* stateName(State s);

    struct Info {
        std::string rv_fw, model_version, model_sha256, model_path;
        bool model_loaded = false;
    };

    // Snapshot for the OLED.
    struct View {
        State  state = IDLE;
        int    q = -1;                 // window quality score 0-100, -1 = unavailable
        double usable_s = 0, req_s = 0, progress = 0;
        bool   model_ok = true, link_ok = true, storage_low = false;
    };

    SessionEngine(RecordingStore& store, PcgStream* link, const Info& info);
    ~SessionEngine();

    void setEventSink(ControlServer* srv) { events_ = srv; }
    void registerOps(ControlServer& srv);
    void loadState();                  // active dataset from disk
    void recoverPartials();            // finish sessions interrupted by a crash/reboot

    // --- audio thread ---
    void onAudio(uint64_t abs, const int16_t* pcm, size_t n);
    void onBeat(const SegQuality::Beat& b);                       // frames are stream-absolute
    void onScored(uint64_t scored_end_abs, double conf, double pnoise);
    void onGap(const PcgStream::Gap& g);
    void tick();
    View view();

private:
    struct BeatRec {
        uint64_t start, end;            // absolute samples
        double   conf, pnoise;
        bool     usable;
        const char* reason;
    };
    struct ScoreRec { uint64_t end; double conf, pnoise; };
    struct GapRec { uint64_t at; uint32_t samples; const char* reason; };

    // control ops (lock held by caller wrappers)
    void opHello(ControlServer::Ctx& c);
    void opStatus(ControlServer::Ctx& c);
    void opStats(ControlServer::Ctx& c);
    void opSetDataset(ControlServer::Ctx& c);
    void opPrepare(ControlServer::Ctx& c);
    void opForceStart(ControlServer::Ctx& c);
    void opStop(ControlServer::Ctx& c);
    void opCancel(ControlServer::Ctx& c);
    void opManifest(ControlServer::Ctx& c);
    void opAck(ControlServer::Ctx& c);
    void opDelete(ControlServer::Ctx& c);
    void opClear(ControlServer::Ctx& c);
    void opLogs(ControlServer::Ctx& c);

    void setState(State s, const char* reason);
    bool startRecording(uint64_t from_abs, bool forced);
    void writeAudio(uint64_t upto_abs);
    void finalize(const char* completion, const char* reason);
    void writeQualityCsv(FILE* f) const;
    cJSON* buildMeta(const char* completion) const;
    bool criteriaMet(cJSON* out) const;
    void addStatusFields(cJSON* o) const;
    void emitStatus();
    void log(const char* fmt, ...);
    int64_t utcNowMs() const;
    void persistDataset();

    RecordingStore& store_;
    PcgStream* link_;
    ControlServer* events_ = nullptr;
    Info info_;
    std::mutex mu_;

    State state_ = IDLE;
    const char* reason_ = "";
    ActiveDataset ds_;

    // pre-trigger ring (absolute indexed)
    std::vector<int16_t> pre_;
    uint64_t now_abs_ = 0;             // one past the newest sample seen

    // session identity
    std::string rid_, subject_id_;
    int take_ = 0;
    Policy pol_;                        // snapshot at prepare
    int pol_rev_ = 0;
    uint64_t arm_abs_ = 0;
    bool forced_ = false;

    // utc: offset from steady clock, set by the app
    int64_t utc_offset_ms_ = 0;
    bool utc_known_ = false;

    // recording
    FILE* wav_ = nullptr;
    FILE* beats_ = nullptr;
    uint64_t rec_start_abs_ = 0, written_abs_ = 0;
    int64_t rec_start_utc_ms_ = 0;
    uint64_t clipped_ = 0;
    std::chrono::steady_clock::time_point last_flush_;

    // quality accounting (current session)
    std::vector<BeatRec> beats_rec_;
    std::vector<ScoreRec> scores_;
    std::vector<GapRec> gaps_;
    uint64_t scored_end_abs_ = 0;
    double usable_ms_ = 0;
    uint64_t usable_end_ = 0;            // union bookkeeping
    int usable_beats_ = 0, total_beats_ = 0;
    int run_beats_ = 0, longest_run_beats_ = 0;
    double run_ms_ = 0, longest_run_ms_ = 0;
    uint64_t run_end_ = 0, run_start_ = 0;
    double conf_sum_ = 0;
    double last_beat_conf_ = -1;
    int q_ = -1;                         // live window quality score
    double last_pnoise_ = 0;

    // armed qualification (contiguous usable coverage)
    uint64_t arm_run_start_ = 0, arm_run_end_ = 0;
    double arm_run_ms_ = 0;

    std::chrono::steady_clock::time_point last_status_;
    std::deque<std::string> logs_;
    const std::chrono::steady_clock::time_point boot_ = std::chrono::steady_clock::now();
};
