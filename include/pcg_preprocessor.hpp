#pragma once

#include <cstdint>
#include <cstddef>
#include <cmath>

// Bandpass + DC-block + RMS-normalize pipeline for 16-bit mono PCM.
// Converts int16 samples to float [-1, 1] and applies filtering.
// Designed for 8 kHz sampling, but works at any rate.
class PcgPreprocessor {
public:
    struct Config {
        int   sample_rate = 8000;
        float low_hz      = 20.0f;
        float high_hz     = 500.0f;
        bool  normalize   = true;
        float target_rms  = 0.5f;
    };

    explicit PcgPreprocessor(const Config& cfg);

    // Process `count` int16 samples into pre-allocated float buffer.
    // in/out buffers must be at least `count` elements.
    void process(const int16_t* in, float* out, size_t count);

    // Reset filter state (e.g. between recordings).
    void reset();

private:
    // Single biquad IIR filter (direct form I).
    struct Biquad {
        float b0 = 1, b1 = 0, b2 = 0;
        float a1 = 0, a2 = 0;
        float x1 = 0, x2 = 0, y1 = 0, y2 = 0;

        void set(const float& b0, const float& b1, const float& b2,
                 const float& a1, const float& a2);
        void reset();
        void process(const float* in, float* out, size_t n);
    };

    Config cfg_;

    // DC blocker (1-pole high-pass)
    // y[n] = x[n] - x[n-1] + r * y[n-1]
    struct DcBlocker {
        float r = 0;
        float x1 = 0, y1 = 0;

        void set(float fc, float sample_rate);
        void reset();
        void process(const float* in, float* out, size_t n);
    };

    DcBlocker dc_blocker_;

    // Butterworth low-pass
    Biquad lp_;
    // Butterworth high-pass
    Biquad hp_;
};
