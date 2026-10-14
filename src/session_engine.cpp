#include "session_engine.hpp"

#include <unistd.h>

#include <algorithm>
#include <cmath>
#include <cstdarg>
#include <cstring>
#include <ctime>
#include <fstream>
#include <iostream>
#include <sstream>

#include "sha256.hpp"

static constexpr int kRate = 8000;
static constexpr int kSamplesPerFrame = 160;         // 50 fps decoder frames
static constexpr size_t kPreTrigger = 16 * kRate;    // 16 s: qualifying run + decoder latency
static constexpr uint64_t kRunGap = 800;             // 0.1 s: consecutive beats stay one run
static constexpr const char* kStateDir = "/root/pcg_state";
static constexpr const char* kScoreDef = "wqs_v2";

const char* SessionEngine::stateName(State s) {
    static const char* n[] = {"IDLE", "CONFIGURED", "ARMED", "RECORDING", "FINALIZING", "DONE",
                              "INCOMPLETE", "CANCELLED", "INTERRUPTED", "STORAGE_FULL", "MODEL_ERROR"};
    return n[s];
}

static bool isUuid(const char* s) {
    if (!s) return false;
    const size_t n = std::strlen(s);
    if (n < 8 || n > 64) return false;
    for (size_t i = 0; i < n; i++) {
        const char c = s[i];
        if (!((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f') || c == '-')) return false;
    }
    return true;
}

static std::string isoUtc(int64_t ms) {
    if (ms <= 0) return "";
    time_t t = static_cast<time_t>(ms / 1000);
    struct tm tm;
    gmtime_r(&t, &tm);
    char buf[80];
    std::snprintf(buf, sizeof(buf), "%04d-%02d-%02dT%02d:%02d:%02d.%03dZ", tm.tm_year + 1900, tm.tm_mon + 1,
                  tm.tm_mday, tm.tm_hour, tm.tm_min, tm.tm_sec, static_cast<int>(ms % 1000));
    return buf;
}

// --- WAV helpers -----------------------------------------------------------

static void writeWavHeader(FILE* f, uint32_t data_bytes) {
    uint8_t h[44];
    auto u32 = [&](int o, uint32_t v) { for (int i = 0; i < 4; i++) h[o + i] = static_cast<uint8_t>(v >> (8 * i)); };
    auto u16 = [&](int o, uint16_t v) { h[o] = static_cast<uint8_t>(v); h[o + 1] = static_cast<uint8_t>(v >> 8); };
    std::memcpy(h, "RIFF", 4);
    u32(4, 36 + data_bytes);
    std::memcpy(h + 8, "WAVEfmt ", 8);
    u32(16, 16);
    u16(20, 1);
    u16(22, 1);
    u32(24, kRate);
    u32(28, kRate * 2);
    u16(32, 2);
    u16(34, 16);
    std::memcpy(h + 36, "data", 4);
    u32(40, data_bytes);
    const long pos = std::ftell(f);
    std::fseek(f, 0, SEEK_SET);
    std::fwrite(h, 1, 44, f);
    if (pos > 44) std::fseek(f, pos, SEEK_SET);
}

static void syncClose(FILE*& f) {
    if (!f) return;
    std::fflush(f);
    ::fsync(fileno(f));
    std::fclose(f);
    f = nullptr;
}

// ---------------------------------------------------------------------------

SessionEngine::SessionEngine(RecordingStore& store, PcgStream* link, const Info& info)
    : store_(store), link_(link), info_(info), pre_(kPreTrigger, 0) {
    fsutil::mkdirs(kStateDir);
    if (!info_.model_loaded) state_ = MODEL_ERROR;
}

SessionEngine::~SessionEngine() {
    std::lock_guard<std::mutex> l(mu_);
    if (state_ == RECORDING) finalize("incomplete", "SESSION_INTERRUPTED");
}

void SessionEngine::log(const char* fmt, ...) {
    char buf[256];
    va_list ap;
    va_start(ap, fmt);
    std::vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    const double t = std::chrono::duration<double>(std::chrono::steady_clock::now() - boot_).count();
    char line[300];
    std::snprintf(line, sizeof(line), "[%8.1f] %s", t, buf);
    std::cout << "[session] " << buf << std::endl;
    logs_.emplace_back(line);
    if (logs_.size() > 200) logs_.pop_front();
}

int64_t SessionEngine::utcNowMs() const {
    const int64_t steady = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now().time_since_epoch()).count();
    return utc_known_ ? steady + utc_offset_ms_ : 0;
}

void SessionEngine::setState(State s, const char* reason) {
    if (s == state_ && std::strcmp(reason, reason_) == 0) return;
    log("state %s -> %s (%s)", stateName(state_), stateName(s), reason);
    state_ = s;
    reason_ = reason;
    emitStatus();
}

// --- persistence -----------------------------------------------------------

void SessionEngine::persistDataset() {
    cJSON* o = cJSON_CreateObject();
    cJSON_AddStringToObject(o, "dataset_id", ds_.id.c_str());
    cJSON_AddStringToObject(o, "name", ds_.name.c_str());
    cJSON_AddNumberToObject(o, "rev", ds_.rev);
    cJSON_AddItemToObject(o, "settings", ds_.policy.toJson());
    char* t = cJSON_PrintUnformatted(o);
    fsutil::writeFileAtomic(std::string(kStateDir) + "/dataset.json", t);
    cJSON_free(t);
    cJSON_Delete(o);
}

void SessionEngine::loadState() {
    std::lock_guard<std::mutex> l(mu_);
    std::string txt;
    if (!fsutil::readFile(std::string(kStateDir) + "/dataset.json", txt)) return;
    cJSON* o = cJSON_Parse(txt.c_str());
    if (!o) return;
    std::string err;
    Policy p;
    if (Policy::fromJson(cJSON_GetObjectItem(o, "settings"), p, err)) {
        ds_.id = ControlServer::str(o, "dataset_id", "");
        ds_.name = ControlServer::str(o, "name", "");
        ds_.rev = static_cast<int>(ControlServer::num(o, "rev", 0));
        ds_.policy = p;
        if (ds_.valid() && state_ == IDLE) state_ = CONFIGURED;
        log("active dataset rev %d loaded", ds_.rev);
    }
    cJSON_Delete(o);
}

// --- audio thread ----------------------------------------------------------

void SessionEngine::onAudio(uint64_t abs, const int16_t* pcm, size_t n) {
    std::lock_guard<std::mutex> l(mu_);
    const size_t cap = pre_.size();
    if (abs > now_abs_) {           // dropped samples (overrun): keep the axis, fill zeros
        for (uint64_t i = now_abs_; i < abs && i < now_abs_ + cap; i++) pre_[i % cap] = 0;
    }
    for (size_t i = 0; i < n; i++) pre_[(abs + i) % cap] = pcm[i];
    now_abs_ = abs + n;
    if (state_ == RECORDING) writeAudio(now_abs_);
}

void SessionEngine::writeAudio(uint64_t upto) {
    if (!wav_) return;
    const size_t cap = pre_.size();
    if (upto > written_abs_ + cap) written_abs_ = upto - cap;   // cannot happen at 100 ms cadence
    while (written_abs_ < upto) {
        const size_t pos = written_abs_ % cap;
        const size_t n = static_cast<size_t>(std::min<uint64_t>(upto - written_abs_, cap - pos));
        for (size_t i = 0; i < n; i++)
            if (pre_[pos + i] >= 32000 || pre_[pos + i] <= -32000) clipped_++;
        std::fwrite(&pre_[pos], sizeof(int16_t), n, wav_);
        written_abs_ += n;
    }
}

void SessionEngine::onScored(uint64_t end, double conf, double pnoise) {
    std::lock_guard<std::mutex> l(mu_);
    q_ = static_cast<int>(std::lround(100.0 * conf));
    last_pnoise_ = pnoise;
    scored_end_abs_ = end;
    scores_.push_back({end, conf, pnoise});
    if (state_ != RECORDING && scores_.size() > 32) scores_.erase(scores_.begin());
}

void SessionEngine::onGap(const PcgStream::Gap& g) {
    std::lock_guard<std::mutex> l(mu_);
    const char* r = g.reason == PcgStream::GAP_SEQ ? "UART_SEQUENCE_GAP"
                  : g.reason == PcgStream::GAP_OVERRUN ? "AUDIO_BUFFER_OVERRUN" : "UART_DISCONTINUITY";
    if (state_ == ARMED) arm_run_ms_ = 0;
    if (state_ == RECORDING && g.at_sample >= rec_start_abs_) gaps_.push_back({g.at_sample, g.samples, r});
}

void SessionEngine::onBeat(const SegQuality::Beat& b) {
    std::lock_guard<std::mutex> l(mu_);
    const Policy& p = (state_ == ARMED || state_ == RECORDING) ? pol_ : ds_.policy;
    BeatRec r;
    r.start = static_cast<uint64_t>(b.start_frame) * kSamplesPerFrame;
    r.end = static_cast<uint64_t>(b.end_frame) * kSamplesPerFrame;
    r.conf = b.conf;
    r.pnoise = b.pnoise;
    r.reason = !b.ok_dur ? "BAD_DURATION" : b.conf < p.conf_threshold ? "LOW_CONF"
             : (p.rhythm_check && !b.ok_rhythm) ? "RHYTHM" : "OK";
    r.usable = std::strcmp(r.reason, "OK") == 0;
    last_beat_conf_ = b.conf;

    if (state_ == ARMED && r.start >= arm_abs_) {
        if (r.usable) {
            if (arm_run_ms_ <= 0 || r.start > arm_run_end_ + kRunGap) {
                arm_run_start_ = r.start;
                arm_run_ms_ = 0;
            }
            arm_run_ms_ += (r.end - r.start) / 8.0;
            arm_run_end_ = r.end;
            if (arm_run_ms_ >= pol_.autostart_s * 1000.0) {
                beats_rec_.push_back(r);   // replayed by startRecording
                startRecording(arm_run_start_, false);
                return;
            }
        } else {
            arm_run_ms_ = 0;
        }
        beats_rec_.push_back(r);
        if (beats_rec_.size() > 64) beats_rec_.erase(beats_rec_.begin());
        return;
    }
    if (state_ != RECORDING || r.start < rec_start_abs_) return;

    total_beats_++;
    conf_sum_ += r.conf;
    if (r.usable) {
        usable_beats_++;
        const uint64_t from = std::max(r.start, usable_end_);
        if (r.end > from) usable_ms_ += (r.end - from) / 8.0;
        usable_end_ = std::max(usable_end_, r.end);
        if (run_beats_ == 0 || r.start > run_end_ + kRunGap) {
            run_beats_ = 0;
            run_ms_ = 0;
            run_start_ = r.start;
        }
        run_beats_++;
        run_ms_ += (r.end - r.start) / 8.0;
        run_end_ = r.end;
        longest_run_beats_ = std::max(longest_run_beats_, run_beats_);
        longest_run_ms_ = std::max(longest_run_ms_, run_ms_);
    } else {
        run_beats_ = 0;
        run_ms_ = 0;
    }
    beats_rec_.push_back(r);
    if (beats_) {
        std::fprintf(beats_, "%llu,%llu,%.4f,%.4f,%d,%s\n",
                     static_cast<unsigned long long>(r.start - rec_start_abs_),
                     static_cast<unsigned long long>(r.end - rec_start_abs_), r.conf, r.pnoise, r.usable ? 1 : 0, r.reason);
    }
    if (criteriaMet(nullptr)) finalize("complete", "TARGET_REACHED");
}

bool SessionEngine::startRecording(uint64_t from, bool forced) {
    const size_t cap = pre_.size();
    if (now_abs_ > cap && from < now_abs_ - cap + 1) from = now_abs_ - cap + 1;
    if (from > now_abs_) from = now_abs_;
    const std::string dir = store_.partialDir(rid_);
    fsutil::mkdirs(dir);
    wav_ = std::fopen((dir + "/audio.wav").c_str(), "w+b");
    beats_ = std::fopen((dir + "/beats.csv").c_str(), "w");
    if (!wav_ || !beats_) {
        log("cannot create files in %s", dir.c_str());
        if (wav_) std::fclose(wav_);
        if (beats_) std::fclose(beats_);
        wav_ = beats_ = nullptr;
        setState(INTERRUPTED, "STORAGE_FULL");
        return false;
    }
    writeWavHeader(wav_, 0);
    std::fprintf(beats_, "start_sample,end_sample,conf,pnoise,usable,reason\n");

    rec_start_abs_ = written_abs_ = from;
    rec_start_utc_ms_ = utc_known_ ? utcNowMs() - static_cast<int64_t>((now_abs_ - from) / 8) : 0;
    forced_ = forced;
    clipped_ = 0;
    usable_ms_ = 0;
    usable_end_ = from;
    usable_beats_ = total_beats_ = run_beats_ = longest_run_beats_ = 0;
    run_ms_ = longest_run_ms_ = conf_sum_ = 0;
    gaps_.clear();

    // session.json lets a reboot recover this session as INTERRUPTED
    cJSON* s = cJSON_CreateObject();
    cJSON_AddStringToObject(s, "recording_id", rid_.c_str());
    cJSON_AddStringToObject(s, "dataset_id", ds_.id.c_str());
    cJSON_AddStringToObject(s, "dataset_name", ds_.name.c_str());
    cJSON_AddStringToObject(s, "subject_id", subject_id_.c_str());
    cJSON_AddNumberToObject(s, "take_index", take_);
    cJSON_AddNumberToObject(s, "policy_revision", pol_rev_);
    cJSON_AddItemToObject(s, "policy", pol_.toJson());
    cJSON_AddNumberToObject(s, "start_utc_ms", static_cast<double>(rec_start_utc_ms_));
    cJSON_AddBoolToObject(s, "force_started", forced);
    cJSON_AddNumberToObject(s, "pretrigger_samples", static_cast<double>(now_abs_ - from));
    char* t = cJSON_PrintUnformatted(s);
    fsutil::writeFileAtomic(dir + "/session.json", t);
    cJSON_free(t);
    cJSON_Delete(s);

    // replay beats already decoded inside the recording (the qualifying run)
    std::vector<BeatRec> earlier;
    earlier.swap(beats_rec_);
    std::vector<ScoreRec> sc;
    for (const auto& r : scores_) if (r.end > from) sc.push_back(r);
    scores_.swap(sc);
    state_ = RECORDING;
    reason_ = forced ? "FORCE_START" : "RELIABLE_AUDIO";
    log("recording %s from %.2f s before now (%s)", rid_.c_str(), (now_abs_ - from) / 8000.0, reason_);
    writeAudio(now_abs_);
    for (const auto& r : earlier) {
        if (r.start < from) continue;
        // re-run the accounting path without re-locking
        total_beats_++;
        conf_sum_ += r.conf;
        if (r.usable) {
            usable_beats_++;
            const uint64_t f2 = std::max(r.start, usable_end_);
            if (r.end > f2) usable_ms_ += (r.end - f2) / 8.0;
            usable_end_ = std::max(usable_end_, r.end);
            if (run_beats_ == 0 || r.start > run_end_ + kRunGap) { run_beats_ = 0; run_ms_ = 0; run_start_ = r.start; }
            run_beats_++;
            run_ms_ += (r.end - r.start) / 8.0;
            run_end_ = r.end;
            longest_run_beats_ = std::max(longest_run_beats_, run_beats_);
            longest_run_ms_ = std::max(longest_run_ms_, run_ms_);
        } else {
            run_beats_ = 0;
            run_ms_ = 0;
        }
        beats_rec_.push_back(r);
        std::fprintf(beats_, "%llu,%llu,%.4f,%.4f,%d,%s\n", static_cast<unsigned long long>(r.start - from),
                     static_cast<unsigned long long>(r.end - from), r.conf, r.pnoise, r.usable ? 1 : 0, r.reason);
    }
    last_flush_ = std::chrono::steady_clock::now();
    emitStatus();
    return true;
}

void SessionEngine::tick() {
    std::lock_guard<std::mutex> l(mu_);
    const auto now = std::chrono::steady_clock::now();
    if (state_ == RECORDING) {
        const double el_ms = (now_abs_ - rec_start_abs_) / 8.0;
        if (el_ms >= pol_.max_duration_s * 1000.0) {
            const bool time_ok = pol_.target_usable_s <= 0 || usable_ms_ >= pol_.target_usable_s * 1000;
            finalize("incomplete", time_ok ? "INSUFFICIENT_CONTIGUOUS_BEATS" : "INSUFFICIENT_USABLE_TIME");
            return;
        }
        if (now - last_flush_ >= std::chrono::seconds(1)) {
            last_flush_ = now;
            writeWavHeader(wav_, static_cast<uint32_t>((written_abs_ - rec_start_abs_) * 2));
            std::fflush(wav_);
            std::fflush(beats_);
            uint64_t total, free_b;
            if (RecordingStore::diskSpace(store_.root(), total, free_b) && free_b < 2u * 1024 * 1024) {
                finalize("incomplete", "STORAGE_FULL");
                state_ = STORAGE_FULL;
                return;
            }
        }
    }
    const bool active = state_ == ARMED || state_ == RECORDING || state_ == FINALIZING;
    const double hz = active ? (pol_.telemetry_hz > 0 ? pol_.telemetry_hz : 2.0) : 0.2;
    if (now - last_status_ >= std::chrono::milliseconds(static_cast<int>(1000.0 / hz))) emitStatus();
}

bool SessionEngine::criteriaMet(cJSON* out) const {
    bool all = true;
    auto crit = [&](const char* name, bool enabled, bool met) {
        if (!enabled) return;
        all = all && met;
        if (out) cJSON_AddBoolToObject(out, name, met);
    };
    crit("target_usable_s", pol_.target_usable_s > 0, usable_ms_ >= pol_.target_usable_s * 1000.0);
    crit("target_usable_beats", pol_.target_usable_beats > 0, usable_beats_ >= pol_.target_usable_beats);
    crit("min_run_beats", pol_.min_run_beats > 0, longest_run_beats_ >= pol_.min_run_beats);
    crit("min_run_s", pol_.min_run_s > 0, longest_run_ms_ >= pol_.min_run_s * 1000.0);
    return all;
}

// --- finalisation ----------------------------------------------------------

void SessionEngine::writeQualityCsv(FILE* f) const {
    std::fprintf(f, "start_sample,end_sample,label,score,pnoise,reason\n");
    const uint64_t n = written_abs_ - rec_start_abs_;
    for (uint64_t s = 0; s < n; s += kRate) {
        const uint64_t a = rec_start_abs_ + s, b = std::min<uint64_t>(a + kRate, written_abs_);
        // usable-beat coverage and gap coverage of this window
        uint64_t cov = 0, gap = 0;
        for (const auto& r : beats_rec_)
            if (r.usable && r.end > a && r.start < b) cov += std::min(r.end, b) - std::max(r.start, a);
        for (const auto& g : gaps_)
            if (g.at < b && g.at + g.samples > a) gap += std::min<uint64_t>(g.at + g.samples, b) - std::max(g.at, a);
        // score from the inference whose 1 s output range contains the window centre
        const uint64_t mid = (a + b) / 2;
        const ScoreRec* sc = nullptr;
        for (const auto& r : scores_)
            if (r.end > mid && r.end - kRate <= mid) { sc = &r; break; }
        const char* label;
        const char* reason;
        if (gap * 2 >= b - a) { label = "unscored"; reason = "UART_SEQUENCE_GAP"; }
        else if (a >= scored_end_abs_ || (!sc && cov == 0)) { label = "unscored"; reason = "UNSCORED"; }
        else if (cov * 2 >= b - a) { label = "usable"; reason = "OK"; }
        else { label = "unusable"; reason = (sc && sc->pnoise > 0.5) ? "NOISY" : "NO_USABLE_BEAT"; }
        char score[16] = "", pn[16] = "";
        if (sc) {
            std::snprintf(score, sizeof(score), "%d", static_cast<int>(std::lround(100 * sc->conf)));
            std::snprintf(pn, sizeof(pn), "%.3f", sc->pnoise);
        }
        std::fprintf(f, "%llu,%llu,%s,%s,%s,%s\n", static_cast<unsigned long long>(s),
                     static_cast<unsigned long long>(b - rec_start_abs_), label, score, pn, reason);
    }
}

cJSON* SessionEngine::buildMeta(const char* completion) const {
    const uint64_t samples = written_abs_ - rec_start_abs_;
    uint64_t gap_samples = 0;
    for (const auto& g : gaps_) gap_samples += g.samples;
    const uint64_t scored_to = std::min(scored_end_abs_, written_abs_);
    const double scored_ms = scored_to > rec_start_abs_
        ? std::max(0.0, (scored_to - rec_start_abs_ - std::min<uint64_t>(gap_samples, scored_to - rec_start_abs_)) / 8.0) : 0.0;
    const double captured_ms = samples / 8.0;

    cJSON* m = cJSON_CreateObject();
    cJSON_AddNumberToObject(m, "schema_version", 1);
    cJSON_AddStringToObject(m, "recording_id", rid_.c_str());
    cJSON_AddStringToObject(m, "dataset_id", ds_.id.c_str());
    cJSON_AddStringToObject(m, "dataset_name", ds_.name.c_str());
    cJSON_AddStringToObject(m, "subject_id", subject_id_.c_str());
    cJSON_AddNumberToObject(m, "take_index", take_);
    cJSON_AddNumberToObject(m, "start_utc_ms", static_cast<double>(rec_start_utc_ms_));
    cJSON_AddStringToObject(m, "start_time_utc", isoUtc(rec_start_utc_ms_).c_str());
    const int64_t end_ms = rec_start_utc_ms_ > 0 ? rec_start_utc_ms_ + static_cast<int64_t>(captured_ms) : 0;
    cJSON_AddStringToObject(m, "end_time_utc", isoUtc(end_ms).c_str());

    cJSON* a = cJSON_AddObjectToObject(m, "audio");
    cJSON_AddStringToObject(a, "path", "audio.wav");
    cJSON_AddNumberToObject(a, "sample_rate_hz", kRate);
    cJSON_AddNumberToObject(a, "channels", 1);
    cJSON_AddNumberToObject(a, "bits_per_sample", 16);
    cJSON_AddNumberToObject(a, "sample_count", static_cast<double>(samples));
    cJSON_AddStringToObject(a, "sha256", Sha256::ofFile(store_.partialDir(rid_) + "/audio.wav").c_str());

    cJSON* q = cJSON_AddObjectToObject(m, "quality");
    cJSON_AddStringToObject(q, "score_definition", kScoreDef);
    cJSON_AddStringToObject(q, "model_version", info_.model_version.c_str());
    cJSON_AddStringToObject(q, "model_sha256", info_.model_sha256.c_str());
    cJSON_AddNumberToObject(q, "policy_revision", pol_rev_);
    cJSON_AddItemToObject(q, "policy", pol_.toJson());
    cJSON_AddNumberToObject(q, "captured_duration_ms", captured_ms);
    cJSON_AddNumberToObject(q, "scored_duration_ms", scored_ms);
    cJSON_AddNumberToObject(q, "usable_duration_ms", usable_ms_);
    cJSON_AddNumberToObject(q, "usable_ratio", scored_ms > 0 ? usable_ms_ / scored_ms : 0.0);
    cJSON_AddNumberToObject(q, "usable_beats", usable_beats_);
    cJSON_AddNumberToObject(q, "total_beats", total_beats_);
    cJSON_AddNumberToObject(q, "longest_usable_run_ms", longest_run_ms_);
    cJSON_AddNumberToObject(q, "longest_usable_run_beats", longest_run_beats_);
    if (total_beats_ > 0) cJSON_AddNumberToObject(q, "mean_beat_conf", conf_sum_ / total_beats_);
    else cJSON_AddNullToObject(q, "mean_beat_conf");
    cJSON_AddNullToObject(q, "snr_db");    // no validated SNR definition yet
    cJSON_AddNumberToObject(q, "clipping_ratio", samples ? static_cast<double>(clipped_) / samples : 0.0);
    cJSON* crit = cJSON_AddObjectToObject(q, "criteria");
    criteriaMet(crit);

    cJSON* tr = cJSON_AddObjectToObject(m, "transport");
    const PcgStream::Stats ls = link_ ? link_->stats() : PcgStream::Stats();
    cJSON_AddNumberToObject(tr, "uart_frames_ok", static_cast<double>(ls.frames_ok));
    cJSON_AddNumberToObject(tr, "uart_crc_errors", static_cast<double>(ls.crc_errors));
    cJSON_AddNumberToObject(tr, "uart_sequence_gaps", static_cast<double>(ls.seq_gaps));
    cJSON_AddNumberToObject(tr, "audio_buffer_overruns", static_cast<double>(ls.overruns));
    cJSON_AddNumberToObject(tr, "gap_samples_in_recording", static_cast<double>(gap_samples));
    cJSON* g = cJSON_AddArrayToObject(m, "gaps");
    for (const auto& x : gaps_) {
        cJSON* o = cJSON_CreateObject();
        cJSON_AddNumberToObject(o, "start_sample", static_cast<double>(x.at - rec_start_abs_));
        cJSON_AddNumberToObject(o, "samples", x.samples);
        cJSON_AddStringToObject(o, "reason", x.reason);
        cJSON_AddItemToArray(g, o);
    }
    cJSON_AddStringToObject(m, "completion_state", completion);
    cJSON_AddStringToObject(m, "completion_reason", reason_);
    cJSON_AddBoolToObject(m, "force_started", forced_);
    cJSON* fw = cJSON_AddObjectToObject(m, "firmware");
    cJSON_AddStringToObject(fw, "rv", info_.rv_fw.c_str());
    return m;
}

void SessionEngine::finalize(const char* completion, const char* reason) {
    if (state_ != RECORDING) return;
    state_ = FINALIZING;
    reason_ = reason;
    emitStatus();
    writeAudio(now_abs_);
    if (wav_) writeWavHeader(wav_, static_cast<uint32_t>((written_abs_ - rec_start_abs_) * 2));
    syncClose(wav_);
    syncClose(beats_);

    const std::string dir = store_.partialDir(rid_);
    bool ok = true;
    FILE* qf = std::fopen((dir + "/quality.csv").c_str(), "w");
    if (qf) {
        writeQualityCsv(qf);
        syncClose(qf);
    } else {
        ok = false;
    }
    cJSON* meta = buildMeta(completion);
    char* t = cJSON_Print(meta);
    ok = ok && fsutil::writeFileAtomic(dir + "/meta.json", t);
    cJSON_free(t);
    cJSON_Delete(meta);
    ::unlink((dir + "/session.json").c_str());
    ok = ok && store_.commit(rid_);

    log("finalized %s: %s (%s) usable %.1f s, %d beats, %.1f s captured", rid_.c_str(), completion, reason,
        usable_ms_ / 1000.0, usable_beats_, (written_abs_ - rec_start_abs_) / 8000.0);
    beats_rec_.clear();
    scores_.clear();
    if (!ok) setState(INTERRUPTED, "IO_ERROR");
    else setState(std::strcmp(completion, "complete") == 0 ? DONE : INCOMPLETE, reason);
}

void SessionEngine::recoverPartials() {
    std::lock_guard<std::mutex> l(mu_);
    for (const auto& rid : store_.partials()) {
        const std::string dir = store_.partialDir(rid);
        std::string txt;
        cJSON* s = fsutil::readFile(dir + "/session.json", txt) ? cJSON_Parse(txt.c_str()) : nullptr;
        if (!s) {
            log("dropping partial %s without session.json", rid.c_str());
            fsutil::removeTree(dir);
            continue;
        }
        // Rebuild the session from what reached the disk.
        rid_ = rid;
        ds_.id = ControlServer::str(s, "dataset_id", "");
        ds_.name = ControlServer::str(s, "dataset_name", "");
        subject_id_ = ControlServer::str(s, "subject_id", "");
        take_ = static_cast<int>(ControlServer::num(s, "take_index", 0));
        pol_rev_ = static_cast<int>(ControlServer::num(s, "policy_revision", 0));
        std::string err;
        Policy::fromJson(cJSON_GetObjectItem(s, "policy"), pol_, err);
        rec_start_utc_ms_ = static_cast<int64_t>(ControlServer::num(s, "start_utc_ms", 0));
        forced_ = ControlServer::flag(s, "force_started", false);
        cJSON_Delete(s);

        FILE* w = std::fopen((dir + "/audio.wav").c_str(), "r+b");
        uint64_t bytes = 0;
        if (w) {
            std::fseek(w, 0, SEEK_END);
            const long sz = std::ftell(w);
            bytes = sz > 44 ? static_cast<uint64_t>(sz - 44) & ~1ull : 0;
            writeWavHeader(w, static_cast<uint32_t>(bytes));
            syncClose(w);
        }
        rec_start_abs_ = 0;
        written_abs_ = bytes / 2;
        beats_rec_.clear();
        scores_.clear();
        gaps_.clear();
        usable_ms_ = 0;
        usable_end_ = 0;
        usable_beats_ = total_beats_ = run_beats_ = longest_run_beats_ = 0;
        run_ms_ = longest_run_ms_ = conf_sum_ = 0;
        clipped_ = 0;
        std::ifstream bf(dir + "/beats.csv");
        std::string line;
        std::getline(bf, line);
        while (std::getline(bf, line)) {
            unsigned long long a, b;
            double c, pn;
            int u;
            char reason[32] = "";
            if (std::sscanf(line.c_str(), "%llu,%llu,%lf,%lf,%d,%31s", &a, &b, &c, &pn, &u, reason) < 5) continue;
            BeatRec r{a, b, c, pn, u != 0, u ? "OK" : "LOW_CONF"};
            total_beats_++;
            conf_sum_ += c;
            if (r.usable) {
                usable_beats_++;
                const uint64_t f2 = std::max(r.start, usable_end_);
                if (r.end > f2) usable_ms_ += (r.end - f2) / 8.0;
                usable_end_ = std::max(usable_end_, r.end);
                if (run_beats_ == 0 || r.start > run_end_ + kRunGap) { run_beats_ = 0; run_ms_ = 0; }
                run_beats_++;
                run_ms_ += (r.end - r.start) / 8.0;
                run_end_ = r.end;
                longest_run_beats_ = std::max(longest_run_beats_, run_beats_);
                longest_run_ms_ = std::max(longest_run_ms_, run_ms_);
            } else {
                run_beats_ = 0;
            }
            beats_rec_.push_back(r);
        }
        scored_end_abs_ = beats_rec_.empty() ? 0 : beats_rec_.back().end;
        reason_ = "SESSION_INTERRUPTED";
        FILE* qf = std::fopen((dir + "/quality.csv").c_str(), "w");
        if (qf) {
            writeQualityCsv(qf);
            syncClose(qf);
        }
        cJSON* meta = buildMeta("interrupted");
        char* t = cJSON_Print(meta);
        fsutil::writeFileAtomic(dir + "/meta.json", t);
        cJSON_free(t);
        cJSON_Delete(meta);
        ::unlink((dir + "/session.json").c_str());
        const bool ok = store_.commit(rid);
        log("recovered interrupted recording %s (%.1f s): %s", rid.c_str(), written_abs_ / 8000.0, ok ? "ok" : "FAILED");
        beats_rec_.clear();
        rid_.clear();
    }
    // the active dataset is re-read by loadState()
    ds_ = ActiveDataset();
    reason_ = "";
}

// --- status / telemetry ----------------------------------------------------

void SessionEngine::addStatusFields(cJSON* o) const {
    cJSON_AddStringToObject(o, "state", stateName(state_));
    cJSON_AddStringToObject(o, "rid", rid_.c_str());
    if (q_ >= 0 && info_.model_loaded) cJSON_AddNumberToObject(o, "q", q_);
    else cJSON_AddNullToObject(o, "q");
    cJSON_AddStringToObject(o, "qdef", kScoreDef);
    if (last_beat_conf_ >= 0) cJSON_AddNumberToObject(o, "bc", std::round(last_beat_conf_ * 1000) / 1000);
    else cJSON_AddNullToObject(o, "bc");
    const bool rec = state_ == RECORDING || state_ == FINALIZING || state_ == DONE || state_ == INCOMPLETE;
    const double el = rec ? (written_abs_ - rec_start_abs_) / 8.0 : 0;
    const uint64_t sc_to = std::min(scored_end_abs_, written_abs_);
    cJSON_AddNumberToObject(o, "el_ms", std::round(el));
    cJSON_AddNumberToObject(o, "sc_ms", rec && sc_to > rec_start_abs_ ? std::round((sc_to - rec_start_abs_) / 8.0) : 0);
    cJSON_AddNumberToObject(o, "us_ms", std::round(rec ? usable_ms_ : 0));
    cJSON_AddNumberToObject(o, "req_ms", std::round(pol_.target_usable_s * 1000));
    cJSON_AddNumberToObject(o, "ub", rec ? usable_beats_ : 0);
    cJSON_AddNumberToObject(o, "req_b", pol_.target_usable_beats);
    cJSON_AddNumberToObject(o, "run_b", rec ? run_beats_ : 0);
    cJSON_AddNumberToObject(o, "run_ms", std::round(rec ? run_ms_ : 0));
    cJSON_AddNumberToObject(o, "lrun_b", rec ? longest_run_beats_ : 0);
    cJSON_AddNumberToObject(o, "lrun_ms", std::round(rec ? longest_run_ms_ : 0));
    cJSON_AddNumberToObject(o, "arm_ms", state_ == ARMED ? std::round(arm_run_ms_) : 0);
    const char* rs = reason_;
    if ((state_ == ARMED || state_ == RECORDING) && q_ >= 0 && last_pnoise_ > 0.5) rs = "NOISY";
    if (!info_.model_loaded) rs = "MODEL_UNAVAILABLE";
    cJSON_AddStringToObject(o, "rs", rs);
    cJSON_AddNumberToObject(o, "rev", ds_.rev);
    cJSON_AddStringToObject(o, "ds", ds_.id.c_str());
    if (link_) {
        const auto ls = link_->stats();
        cJSON* l = cJSON_AddObjectToObject(o, "link");
        cJSON_AddNumberToObject(l, "ok", static_cast<double>(ls.frames_ok));
        cJSON_AddNumberToObject(l, "crc", static_cast<double>(ls.crc_errors));
        cJSON_AddNumberToObject(l, "gaps", static_cast<double>(ls.seq_gaps));
        cJSON_AddNumberToObject(l, "ovr", static_cast<double>(ls.overruns));
    }
}

void SessionEngine::emitStatus() {
    last_status_ = std::chrono::steady_clock::now();
    if (!events_) return;
    cJSON* o = cJSON_CreateObject();
    cJSON_AddStringToObject(o, "ev", "status");
    addStatusFields(o);
    events_->sendEvent(o);
    cJSON_Delete(o);
}

SessionEngine::View SessionEngine::view() {
    std::lock_guard<std::mutex> l(mu_);
    View v;
    v.state = state_;
    v.q = info_.model_loaded ? q_ : -1;
    const bool rec = state_ == RECORDING || state_ == FINALIZING || state_ == DONE || state_ == INCOMPLETE;
    v.usable_s = rec ? usable_ms_ / 1000.0 : 0;
    v.req_s = pol_.target_usable_s > 0 ? pol_.target_usable_s : ds_.policy.target_usable_s;
    v.progress = v.req_s > 0 ? std::min(1.0, v.usable_s / v.req_s)
               : (pol_.target_usable_beats > 0 && rec ? std::min(1.0, double(usable_beats_) / pol_.target_usable_beats) : 0);
    v.model_ok = info_.model_loaded;
    if (link_) {
        // audio considered flowing if a frame arrived recently (checked via counters by caller)
        v.link_ok = true;
    }
    uint64_t total, free_b;
    if (RecordingStore::diskSpace(store_.root(), total, free_b)) v.storage_low = free_b < 20u * 1024 * 1024;
    return v;
}

// --- control ops -----------------------------------------------------------

void SessionEngine::registerOps(ControlServer& srv) {
    auto wrap = [this](void (SessionEngine::*fn)(ControlServer::Ctx&)) {
        return [this, fn](ControlServer::Ctx& c) {
            std::lock_guard<std::mutex> l(mu_);
            const double utc = ControlServer::num(c.req, "utc_ms", 0);
            if (utc > 1.6e12) {     // app supplies wall-clock time (the board has no RTC)
                const int64_t steady = std::chrono::duration_cast<std::chrono::milliseconds>(
                    std::chrono::steady_clock::now().time_since_epoch()).count();
                utc_offset_ms_ = static_cast<int64_t>(utc) - steady;
                utc_known_ = true;
            }
            (this->*fn)(c);
        };
    };
    srv.on("hello", wrap(&SessionEngine::opHello));
    srv.on("status", wrap(&SessionEngine::opStatus), false);
    srv.on("stats", wrap(&SessionEngine::opStats), false);
    srv.on("set_dataset", wrap(&SessionEngine::opSetDataset));
    srv.on("prepare", wrap(&SessionEngine::opPrepare));
    srv.on("force_start", wrap(&SessionEngine::opForceStart));
    srv.on("stop", wrap(&SessionEngine::opStop));
    srv.on("cancel", wrap(&SessionEngine::opCancel));
    srv.on("manifest", wrap(&SessionEngine::opManifest), false);
    srv.on("ack", wrap(&SessionEngine::opAck));
    srv.on("delete", wrap(&SessionEngine::opDelete));
    srv.on("clear", wrap(&SessionEngine::opClear));
    srv.on("logs", wrap(&SessionEngine::opLogs), false);
}

void SessionEngine::opHello(ControlServer::Ctx& c) {
    cJSON_AddStringToObject(c.rsp, "rv_fw", info_.rv_fw.c_str());
    cJSON_AddNumberToObject(c.rsp, "ctrl_proto", 1);
    cJSON_AddNumberToObject(c.rsp, "link_proto", PcgStream::kLinkProto);
    cJSON_AddNumberToObject(c.rsp, "audio_proto", PcgStream::kAudioProto);
    cJSON* m = cJSON_AddObjectToObject(c.rsp, "model");
    cJSON_AddStringToObject(m, "version", info_.model_version.c_str());
    cJSON_AddStringToObject(m, "sha256", info_.model_sha256.c_str());
    cJSON_AddBoolToObject(m, "loaded", info_.model_loaded);
    cJSON* d = cJSON_AddObjectToObject(c.rsp, "dataset");
    cJSON_AddStringToObject(d, "id", ds_.id.c_str());
    cJSON_AddStringToObject(d, "name", ds_.name.c_str());
    cJSON_AddNumberToObject(d, "rev", ds_.rev);
    cJSON* caps = cJSON_AddArrayToObject(c.rsp, "caps");
    for (const char* cap : {"record", "autostart", "force_start", "manifest", "file_read", "ack_delete", "stats"})
        cJSON_AddItemToArray(caps, cJSON_CreateString(cap));
}

void SessionEngine::opStatus(ControlServer::Ctx& c) { addStatusFields(c.rsp); }

static double readProcNumber(const char* path, const char* key) {
    std::ifstream f(path);
    std::string line;
    while (std::getline(f, line)) {
        if (line.compare(0, std::strlen(key), key) == 0) return std::atof(line.c_str() + std::strlen(key));
    }
    return -1;
}

void SessionEngine::opStats(ControlServer::Ctx& c) {
    static uint64_t prev_busy = 0, prev_total = 0;
    cJSON_AddNumberToObject(c.rsp, "utc_ms", static_cast<double>(utcNowMs()));
    std::ifstream up("/proc/uptime");
    double sys_up = 0;
    up >> sys_up;
    cJSON_AddNumberToObject(c.rsp, "uptime_s", std::round(sys_up));
    cJSON_AddNumberToObject(c.rsp, "process_uptime_s",
                            std::round(std::chrono::duration<double>(std::chrono::steady_clock::now() - boot_).count()));
    cJSON* mem = cJSON_AddObjectToObject(c.rsp, "mem_kb");
    cJSON_AddNumberToObject(mem, "total", readProcNumber("/proc/meminfo", "MemTotal:"));
    cJSON_AddNumberToObject(mem, "available", readProcNumber("/proc/meminfo", "MemAvailable:"));
    cJSON_AddNumberToObject(mem, "process_rss", readProcNumber("/proc/self/status", "VmRSS:"));
    // CPU utilisation since the previous stats call (from /proc/stat counters)
    std::ifstream st("/proc/stat");
    std::string cpu;
    uint64_t v[8] = {};
    st >> cpu >> v[0] >> v[1] >> v[2] >> v[3] >> v[4] >> v[5] >> v[6] >> v[7];
    const uint64_t total = v[0] + v[1] + v[2] + v[3] + v[4] + v[5] + v[6] + v[7];
    const uint64_t busy = total - v[3] - v[4];
    if (prev_total && total > prev_total)
        cJSON_AddNumberToObject(c.rsp, "cpu_pct", std::round(1000.0 * (busy - prev_busy) / (total - prev_total)) / 10);
    else
        cJSON_AddNullToObject(c.rsp, "cpu_pct");
    prev_busy = busy;
    prev_total = total;
    cJSON_AddStringToObject(c.rsp, "npu_pct", "unsupported");
    const double temp = readProcNumber("/sys/class/thermal/thermal_zone0/temp", "");
    if (temp > 0) cJSON_AddNumberToObject(c.rsp, "temp_c", temp / 1000.0);
    else cJSON_AddStringToObject(c.rsp, "temp_c", "unsupported");

    uint64_t tot_b = 0, free_b = 0;
    RecordingStore::diskSpace(store_.root(), tot_b, free_b);
    cJSON* sto = cJSON_AddObjectToObject(c.rsp, "storage");
    cJSON_AddNumberToObject(sto, "total_bytes", static_cast<double>(tot_b));
    cJSON_AddNumberToObject(sto, "free_bytes", static_cast<double>(free_b));
    const double reserve = 20.0 * 1024 * 1024;
    // 16 000 B/s PCM + ~3 % sidecar/header overhead
    cJSON_AddNumberToObject(sto, "est_remaining_s", std::max(0.0, std::floor((free_b - reserve) / (16000.0 * 1.03))));
    const auto cnt = store_.counts();
    cJSON* rc = cJSON_AddObjectToObject(c.rsp, "recordings");
    cJSON_AddNumberToObject(rc, "count", cnt.total);
    cJSON_AddNumberToObject(rc, "pending_transfer", cnt.unverified);
    cJSON_AddNumberToObject(rc, "incomplete", cnt.incomplete);
    cJSON_AddNumberToObject(rc, "total_ms", cnt.total_ms);
    if (link_) {
        const auto ls = link_->stats();
        cJSON* u = cJSON_AddObjectToObject(c.rsp, "uart");
        cJSON_AddNumberToObject(u, "baud", ls.baud);
        cJSON_AddNumberToObject(u, "frames_ok", static_cast<double>(ls.frames_ok));
        cJSON_AddNumberToObject(u, "crc_errors", static_cast<double>(ls.crc_errors));
        cJSON_AddNumberToObject(u, "malformed", static_cast<double>(ls.malformed));
        cJSON_AddNumberToObject(u, "seq_gaps", static_cast<double>(ls.seq_gaps));
        cJSON_AddNumberToObject(u, "overruns", static_cast<double>(ls.overruns));
        cJSON_AddNumberToObject(u, "bytes", static_cast<double>(ls.bytes_read));
        cJSON_AddNumberToObject(u, "since_s", std::round(std::chrono::duration<double>(std::chrono::steady_clock::now() - boot_).count()));
    }
    cJSON* m = cJSON_AddObjectToObject(c.rsp, "model");
    cJSON_AddBoolToObject(m, "loaded", info_.model_loaded);
    cJSON_AddStringToObject(m, "version", info_.model_version.c_str());
    cJSON_AddStringToObject(m, "sha256", info_.model_sha256.c_str());
}

void SessionEngine::opSetDataset(ControlServer::Ctx& c) {
    if (state_ == ARMED || state_ == RECORDING || state_ == FINALIZING)
        return ControlServer::reject(c, "BUSY", "SESSION_ACTIVE");
    const char* id = ControlServer::str(c.req, "dataset_id");
    const char* name = ControlServer::str(c.req, "name", "");
    const double rev = ControlServer::num(c.req, "rev", -1);
    if (!isUuid(id)) return ControlServer::reject(c, "INVALID", "dataset_id");
    if (rev < 0) return ControlServer::reject(c, "INVALID", "rev");
    Policy p;
    std::string err;
    if (!Policy::fromJson(cJSON_GetObjectItem(c.req, "settings"), p, err)) {
        ControlServer::reject(c, "INVALID", "SETTINGS");
        cJSON_AddStringToObject(c.rsp, "field", err.c_str());
        return;
    }
    ds_.id = id;
    ds_.name = std::string(name).substr(0, 64);
    ds_.rev = static_cast<int>(rev);
    ds_.policy = p;
    persistDataset();
    if (state_ == IDLE || state_ == DONE || state_ == INCOMPLETE || state_ == CANCELLED || state_ == INTERRUPTED ||
        state_ == STORAGE_FULL || state_ == CONFIGURED)
        setState(CONFIGURED, "DATASET_ACTIVE");
    log("dataset rev %d active", ds_.rev);
    cJSON_AddNumberToObject(c.rsp, "rev", ds_.rev);
    cJSON_AddItemToObject(c.rsp, "settings", p.toJson());
}

void SessionEngine::opPrepare(ControlServer::Ctx& c) {
    const char* rid = ControlServer::str(c.req, "recording_id");
    if ((state_ == ARMED || state_ == RECORDING || state_ == FINALIZING)) {
        if (rid && rid_ == rid) { cJSON_AddStringToObject(c.rsp, "state", stateName(state_)); return; }   // idempotent
        return ControlServer::reject(c, "BUSY", "SESSION_ACTIVE");
    }
    if (!info_.model_loaded) return ControlServer::reject(c, "REJECTED", "MODEL_UNAVAILABLE");
    const char* ds = ControlServer::str(c.req, "dataset_id");
    const char* subj = ControlServer::str(c.req, "subject_id");
    if (!isUuid(rid) || !isUuid(subj) || !isUuid(ds)) return ControlServer::reject(c, "INVALID", "ID");
    if (!ds_.valid() || ds_.id != ds) return ControlServer::reject(c, "REJECTED", "DATASET_MISMATCH");
    if (static_cast<int>(ControlServer::num(c.req, "rev", -1)) != ds_.rev) return ControlServer::reject(c, "REJECTED", "SETTINGS_MISMATCH");
    std::string p, sha;
    if (store_.resolve(rid, "meta", p, sha)) return ControlServer::reject(c, "REJECTED", "DUPLICATE_RECORDING");
    uint64_t total, free_b;
    const double need = ds_.policy.max_duration_s * 16000.0 * 1.1 + 4.0 * 1024 * 1024;
    if (RecordingStore::diskSpace(store_.root(), total, free_b) && free_b < need) {
        setState(STORAGE_FULL, "STORAGE_FULL");
        return ControlServer::reject(c, "REJECTED", "STORAGE_FULL");
    }
    rid_ = rid;
    subject_id_ = subj;
    take_ = static_cast<int>(ControlServer::num(c.req, "take", 1));
    pol_ = ds_.policy;
    pol_rev_ = ds_.rev;
    arm_abs_ = now_abs_;
    arm_run_ms_ = 0;
    beats_rec_.clear();
    usable_ms_ = 0;
    usable_beats_ = total_beats_ = 0;
    forced_ = false;
    if (pol_.autostart_s <= 0) {
        state_ = ARMED;
        startRecording(now_abs_, false);
    } else {
        setState(ARMED, "WAITING_FOR_QUALITY");
    }
    cJSON_AddStringToObject(c.rsp, "state", stateName(state_));
}

void SessionEngine::opForceStart(ControlServer::Ctx& c) {
    const char* rid = ControlServer::str(c.req, "recording_id", "");
    if (rid_ != rid) return ControlServer::reject(c, "REJECTED", "UNKNOWN_RECORDING");
    if (state_ == RECORDING || state_ == FINALIZING || state_ == DONE) {
        cJSON_AddStringToObject(c.rsp, "state", stateName(state_));
        return;
    }
    if (state_ != ARMED) return ControlServer::reject(c, "REJECTED", "NOT_ARMED");
    startRecording(now_abs_, true);
    cJSON_AddStringToObject(c.rsp, "state", stateName(state_));
}

void SessionEngine::opStop(ControlServer::Ctx& c) {
    const char* rid = ControlServer::str(c.req, "recording_id", "");
    if (rid_ != rid) return ControlServer::reject(c, "REJECTED", "UNKNOWN_RECORDING");
    if (state_ == ARMED) setState(CANCELLED, "USER_CANCELLED");
    else if (state_ == RECORDING) finalize(criteriaMet(nullptr) ? "complete" : "incomplete", "USER_STOPPED");
    cJSON_AddStringToObject(c.rsp, "state", stateName(state_));
}

void SessionEngine::opCancel(ControlServer::Ctx& c) {
    const char* rid = ControlServer::str(c.req, "recording_id", "");
    if (rid_ != rid) return ControlServer::reject(c, "REJECTED", "UNKNOWN_RECORDING");
    if (state_ == RECORDING || state_ == FINALIZING) return ControlServer::reject(c, "REJECTED", "RECORDING_ACTIVE");
    if (state_ == ARMED) setState(CANCELLED, "USER_CANCELLED");
    cJSON_AddStringToObject(c.rsp, "state", stateName(state_));
}

void SessionEngine::opManifest(ControlServer::Ctx& c) {
    const int offset = std::max(0, static_cast<int>(ControlServer::num(c.req, "offset", 0)));
    const int limit = std::min(6, std::max(1, static_cast<int>(ControlServer::num(c.req, "limit", 6))));   // fits a 4 KB frame
    cJSON* m = store_.manifest(offset, limit);
    cJSON* it = m->child;
    while (it) {
        cJSON* next = it->next;
        cJSON_DetachItemViaPointer(m, it);
        cJSON_AddItemToObject(c.rsp, it->string, it);
        it = next;
    }
    cJSON_Delete(m);
}

void SessionEngine::opAck(ControlServer::Ctx& c) {
    const char* rid = ControlServer::str(c.req, "recording_id", "");
    const char* sha = ControlServer::str(c.req, "sha256", "");
    const std::string err = store_.ack(rid, sha, utcNowMs());
    if (!err.empty()) return ControlServer::reject(c, "REJECTED", err.c_str());
    log("recording %s verified by phone", rid);
    cJSON_AddStringToObject(c.rsp, "transfer_state", "verified");
}

void SessionEngine::opDelete(ControlServer::Ctx& c) {
    const char* rid = ControlServer::str(c.req, "recording_id", "");
    const char* sha = ControlServer::str(c.req, "sha256", "");
    if (rid_ == rid && (state_ == RECORDING || state_ == FINALIZING)) return ControlServer::reject(c, "BUSY", "RECORDING_ACTIVE");
    const std::string err = store_.remove(rid, sha);
    if (!err.empty()) return ControlServer::reject(c, "REJECTED", err.c_str());
    log("recording %s deleted after verified transfer", rid);
    cJSON_AddBoolToObject(c.rsp, "deleted", true);
}

void SessionEngine::opClear(ControlServer::Ctx& c) {
    if (state_ == RECORDING || state_ == FINALIZING) return ControlServer::reject(c, "BUSY", "RECORDING_ACTIVE");
    bool force = ControlServer::flag(c.req, "force", false);
    // Destructive override is a device-side policy, not something the app can grant itself.
    if (force && ::access((std::string(kStateDir) + "/allow_force_clear").c_str(), F_OK) != 0) {
        return ControlServer::reject(c, "REJECTED", "FORCE_DISABLED");
    }
    cJSON* r = store_.clear(force);
    cJSON_AddItemToObject(c.rsp, "deleted", cJSON_DetachItemFromObject(r, "deleted"));
    cJSON_AddItemToObject(c.rsp, "kept", cJSON_DetachItemFromObject(r, "kept"));
    cJSON_Delete(r);
    log("clear (force=%d)", force ? 1 : 0);
}

void SessionEngine::opLogs(ControlServer::Ctx& c) {
    const int n = std::min(40, std::max(1, static_cast<int>(ControlServer::num(c.req, "lines", 20))));
    cJSON* arr = cJSON_AddArrayToObject(c.rsp, "lines");
    const int start = std::max(0, static_cast<int>(logs_.size()) - n);
    for (int i = start; i < static_cast<int>(logs_.size()); i++) cJSON_AddItemToArray(arr, cJSON_CreateString(logs_[i].c_str()));
}
