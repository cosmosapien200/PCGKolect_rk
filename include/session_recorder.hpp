#pragma once

#include <cstdint>
#include <cstdio>
#include <string>

// Records each firmware session for later review / fine-tuning:
//   <dir>/session_NNN.wav          raw int16 mono 8 kHz (exactly what the quality pipeline saw)
//   <dir>/session_NNN_beats.csv    one row per decoded beat (v2, NPU)
//   <dir>/session_NNN_quality.csv  one row per second (v1 score + gates, v2 noise/confidence)
// The WAV header is rewritten every second, so the file stays valid even if the process is killed.
class SessionRecorder {
public:
    ~SessionRecorder() { close(); }

    bool open(const std::string& dir, size_t max_bytes = 100u * 1024 * 1024);
    bool isOpen() const { return wav_ != nullptr; }
    const std::string& name() const { return base_; }

    void writeAudio(const int16_t* pcm, size_t n);
    void logBeat(double t0, double t1, double conf, double pnoise, bool usable, bool gated);
    void logQuality(double t0, double t1, double q, const char* verdict, const char* reason,
                    double level_dbfs, double seg_noise, double seg_conf);
    void close();

private:
    void patchHeader();

    FILE* wav_ = nullptr;
    FILE* beats_ = nullptr;
    FILE* qual_ = nullptr;
    std::string base_;
    size_t data_bytes_ = 0, max_bytes_ = 0, since_patch_ = 0;
};
