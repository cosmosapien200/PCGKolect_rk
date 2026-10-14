#pragma once

#include <cstdint>
#include <cstddef>
#include <vector>

// Real-time PCG signal-quality estimator (model sqi_lr_v1).
//
// Feed raw int16 PCM @ 8 kHz (straight from PcgStream, NOT the normalised
// PcgPreprocessor output — the level/clipping gates need absolute amplitude).
// Internally: 8 kHz -> 2 kHz FIR decimation -> 20 Hz high-pass -> 4 s ring.
// Every 1 s a Result is produced describing the centre 2 s of the last 4 s
// (i.e. audio from 3 s ago to 1 s ago). CPU only, no NPU: ~0.4 ms per second of
// audio on a desktop x86 core (A7 not yet measured; expect well under 1 % CPU).
//
// The Python reference is PCGKolect_model/sqi_frontend.py; model constants
// are generated into sqi_model_params.h by PCGKolect_model/train.py.
class SignalQuality {
public:
    enum Verdict { GOOD = 0, MARGINAL = 1, BAD = 2 };
    enum Reason  { OK = 0, NOISY = 1, NO_CONTACT = 2, TOO_LOUD = 3, CLIPPING = 4 };

    static constexpr int kNumFeatures = 19;

    struct Result {
        double  t_start_s  = 0;   // scored span, seconds since stream start
        double  t_end_s    = 0;
        double  q          = 0;   // model quality score 0..1 (higher = cleaner)
        Verdict verdict    = BAD;
        Reason  reason     = NOISY;
        double  level_dbfs = 0;   // RMS level of the scored span after high-pass
        double  clip_frac  = 0;   // fraction of clipped 8 kHz samples in the span
        double  hr_bpm     = 0;   // rough heart rate (GOOD windows only, else 0)
        double  features[kNumFeatures + 1] = {};  // model features + log_rms
    };

    SignalQuality();

    // Push contiguous 8 kHz samples. Returns the number of new results
    // appended to `out` (0 or more).
    size_t push(const int16_t* pcm, size_t count, std::vector<Result>& out);

    void reset();

    // Exposed for the parity test: features of one 4 s @ 2 kHz buffer.
    static void computeFeatures(const double* x, double* feat);
    static double scoreFeatures(const double* feat);

    static const char* verdictName(Verdict v);
    static const char* reasonName(Reason r);

private:
    static constexpr int kFs      = 2000;
    static constexpr int kDecim   = 4;
    static constexpr int kCtx     = 4 * kFs;   // 4 s buffer
    static constexpr int kStep    = kFs;       // 1 s hop
    static constexpr int kTaps    = 63;

    void pushDecimated(double y, int clipped_in_block, std::vector<Result>& out, size_t& produced);
    void scoreNow(std::vector<Result>& out);

    // FIR decimator state (8 kHz history, circular)
    double fir_hist_[kTaps] = {};
    int    fir_pos_   = 0;
    int    phase_     = 0;
    int    clip_acc_  = 0;
    double pending_   = 0;   // decimated sample awaiting its 4-input clip count

    // High-pass biquad state (direct form II transposed, same as scipy sosfilt)
    double hp_z1_ = 0, hp_z2_ = 0;

    // 2 kHz ring + per-sample clip counts (number of clipped 8 kHz inputs)
    double  ring_[kCtx] = {};
    uint8_t clip_ring_[kCtx] = {};
    int     ring_pos_  = 0;
    long    total_2k_  = 0;
    int     since_step_ = 0;
};

// Aggregates per-second results into session metrics for display / sidecar.
class QualityTracker {
public:
    struct Summary {
        double scored_s      = 0;
        double good_s        = 0;
        double marginal_s    = 0;
        double bad_s         = 0;
        double usable_ratio  = 0;   // good_s / scored_s
        double usable_beats  = 0;   // sum(hr/60) over GOOD seconds
        double longest_run_s = 0;   // longest consecutive GOOD stretch
        double current_run_s = 0;
        double mean_q        = 0;
    };

    void add(const SignalQuality::Result& r, double step_s = 1.0);
    void reset() { s_ = Summary(); q_sum_ = 0; }
    const Summary& summary() const { return s_; }

private:
    Summary s_;
    double  q_sum_ = 0;
};
