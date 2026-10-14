#pragma once

#include <cstddef>
#include <cstdint>
#include <deque>
#include <functional>
#include <vector>

// Segmentation-based PCG quality (model v2): per-beat usability from an NPU segmentation net.
//
//   int16 @ 8 kHz -> FIR decimate to 2 kHz -> 20 Hz HP -> ring (5.164 s)
//   every 1 s: log-filterbank features [32 x 256] -> NPU -> frame posteriors
//              (S1, systole, S2, diastole, noise) for the frames that now have >= 1.28 s
//              of future context
//   -> posterior history (20 s) -> duration-constrained Viterbi -> beats -> usability
//
// Python reference: PCGKolect_model/seg/{model.py, decode.py}. Constants: seg_params.h.
// The NPU call is injected (SegModel::run on the board, a file/stub in host tests).
class SegQuality {
public:
    static constexpr int kClasses = 5;

    struct Beat {
        long   start_frame = 0;   // absolute frame index (50 fps) since stream start
        long   end_frame   = 0;
        double t_start_s   = 0, t_end_s = 0, dur_s = 0;
        double conf        = 0;   // mean posterior of the decoded state along the beat
        double pnoise      = 0;   // mean P(noise) along the beat
        double rhythm_dev  = 0;
        bool   ok_dur = false, ok_conf = false, ok_rhythm = false;
        bool   usable      = false;
        double score       = 0;
    };

    // infer(feat[32*256] band-major, probs[256*5]) -> success
    using InferFn = std::function<bool(const float*, float*)>;

    explicit SegQuality(InferFn infer);

    // Push 8 kHz samples. Newly finalised beats are appended to `beats`.
    // Returns the number of NPU inferences run (0 or more).
    int push(const int16_t* pcm, size_t count, std::vector<Beat>& beats);

    // Mean P(noise) / max-state posterior over the most recent posterior frames (1 s).
    double recentNoise() const { return recent_noise_; }
    double recentConf() const { return recent_conf_; }
    double lastInferMs() const { return last_infer_ms_; }
    // Seconds of audio whose posteriors are final (denominator for coverage).
    double scoredSeconds() const { return static_cast<double>(next_frame_) / 50.0; }

    // --- building blocks, exposed for parity tests ---
    // x: kSamples floats @ 2 kHz -> feat[32*256] band-major (same as seg/model.py Frontend)
    static void features(const float* x, float* feat);
    // P: [T][5] -> decoded class per frame
    static void viterbi(const float* P, int T, std::vector<int>& cls);
    // decoded classes + P -> scored beats (frame indices relative to P)
    static void beats(const std::vector<int>& cls, const float* P, int T, std::vector<Beat>& out);

private:
    void step(std::vector<Beat>& out);
    void decode(std::vector<Beat>& out, bool flush);

    InferFn infer_;
    // decimator + HP (same constants as SignalQuality)
    double fir_hist_[63] = {};
    int    fir_pos_ = 0, phase_ = 0;
    double hp_z1_ = 0, hp_z2_ = 0;
    std::vector<float> ring_;      // 2 kHz, kSamples
    int    ring_pos_ = 0;
    long   total_2k_ = 0;
    // posterior history
    std::deque<float> hist_;       // [frames][5]
    long   hist_start_ = 0;        // absolute frame index of hist_[0]
    long   next_frame_ = 0;        // next absolute frame to append
    long   last_emitted_end_ = -1;
    double recent_noise_ = 0, recent_conf_ = 0, last_infer_ms_ = 0;
};

// Session aggregation of beats (what the display / sidecar shows).
class BeatTracker {
public:
    struct Summary {
        int    beats = 0, usable_beats = 0;
        double usable_s = 0;          // total duration of usable beats
        int    run = 0, longest_run = 0;   // consecutive usable beats
        double hr_bpm = 0;            // median of recent usable beat durations
        double usable_ratio = 0;      // usable_beats / beats
    };
    void add(const SegQuality::Beat& b);
    const Summary& summary() const { return s_; }
    void reset() { s_ = Summary(); recent_.clear(); last_end_ = -1; }

private:
    Summary s_;
    long last_end_ = -1;
    std::deque<double> recent_;
};
