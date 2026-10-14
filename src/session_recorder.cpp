#include "session_recorder.hpp"

#include <cstring>
#include <sys/stat.h>

namespace {

void put32(FILE* f, uint32_t v) { uint8_t b[4] = {uint8_t(v), uint8_t(v >> 8), uint8_t(v >> 16), uint8_t(v >> 24)}; std::fwrite(b, 1, 4, f); }
void put16(FILE* f, uint16_t v) { uint8_t b[2] = {uint8_t(v), uint8_t(v >> 8)}; std::fwrite(b, 1, 2, f); }

void writeHeader(FILE* f, uint32_t data_bytes) {
    const uint32_t rate = 8000;
    std::fwrite("RIFF", 1, 4, f); put32(f, 36 + data_bytes);
    std::fwrite("WAVEfmt ", 1, 8, f); put32(f, 16);
    put16(f, 1); put16(f, 1); put32(f, rate); put32(f, rate * 2); put16(f, 2); put16(f, 16);
    std::fwrite("data", 1, 4, f); put32(f, data_bytes);
}

} // namespace

bool SessionRecorder::open(const std::string& dir, size_t max_bytes) {
    ::mkdir(dir.c_str(), 0755);
    for (int i = 1; i < 10000; i++) {
        char name[64];
        std::snprintf(name, sizeof(name), "/session_%03d", i);
        struct stat st;
        if (::stat((dir + name + ".wav").c_str(), &st) != 0) { base_ = dir + name; break; }
    }
    if (base_.empty()) return false;
    wav_ = std::fopen((base_ + ".wav").c_str(), "wb");
    beats_ = std::fopen((base_ + "_beats.csv").c_str(), "w");
    qual_ = std::fopen((base_ + "_quality.csv").c_str(), "w");
    if (!wav_ || !beats_ || !qual_) { close(); return false; }
    writeHeader(wav_, 0);
    std::fprintf(beats_, "t_start_s,t_end_s,conf,pnoise,usable,gated\n");
    std::fprintf(qual_, "t_start_s,t_end_s,v1_q,v1_verdict,v1_reason,level_dbfs,v2_pnoise,v2_conf\n");
    max_bytes_ = max_bytes;
    return true;
}

void SessionRecorder::writeAudio(const int16_t* pcm, size_t n) {
    if (!wav_ || data_bytes_ + n * 2 > max_bytes_) return;   // stop growing at the size cap
    for (size_t i = 0; i < n; i++) put16(wav_, static_cast<uint16_t>(pcm[i]));
    data_bytes_ += n * 2;
    since_patch_ += n;
    if (since_patch_ >= 8000) { patchHeader(); since_patch_ = 0; }
}

void SessionRecorder::logBeat(double t0, double t1, double conf, double pnoise, bool usable, bool gated) {
    if (!beats_) return;
    std::fprintf(beats_, "%.2f,%.2f,%.4f,%.4f,%d,%d\n", t0, t1, conf, pnoise, usable ? 1 : 0, gated ? 1 : 0);
    std::fflush(beats_);
}

void SessionRecorder::logQuality(double t0, double t1, double q, const char* verdict, const char* reason,
                                 double level_dbfs, double seg_noise, double seg_conf) {
    if (!qual_) return;
    std::fprintf(qual_, "%.2f,%.2f,%.4f,%s,%s,%.2f,%.4f,%.4f\n", t0, t1, q, verdict, reason, level_dbfs,
                 seg_noise, seg_conf);
    std::fflush(qual_);
}

void SessionRecorder::patchHeader() {
    std::fflush(wav_);
    std::fseek(wav_, 0, SEEK_SET);
    writeHeader(wav_, static_cast<uint32_t>(data_bytes_));
    std::fseek(wav_, 0, SEEK_END);
    std::fflush(wav_);
}

void SessionRecorder::close() {
    if (wav_) { patchHeader(); std::fclose(wav_); wav_ = nullptr; }
    if (beats_) { std::fclose(beats_); beats_ = nullptr; }
    if (qual_) { std::fclose(qual_); qual_ = nullptr; }
}
