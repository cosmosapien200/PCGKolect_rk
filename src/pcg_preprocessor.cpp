#include "pcg_preprocessor.hpp"

#include <cmath>
#include <algorithm>

static constexpr float PI = 3.14159265358979323846f;

// ---------------------------------------------------------------------------
// Biquad IIR (direct form I)
// ---------------------------------------------------------------------------

void PcgPreprocessor::Biquad::set(const float& b0, const float& b1, const float& b2,
                                   const float& a1, const float& a2) {
    this->b0 = b0; this->b1 = b1; this->b2 = b2;
    this->a1 = a1; this->a2 = a2;
    this->x1 = 0; this->x2 = 0;
    this->y1 = 0; this->y2 = 0;
}

void PcgPreprocessor::Biquad::reset() {
    x1 = x2 = y1 = y2 = 0;
}

void PcgPreprocessor::Biquad::process(const float* in, float* out, size_t n) {
    for (size_t i = 0; i < n; i++) {
        const float y = b0 * in[i] + b1 * x1 + b2 * x2 - a1 * y1 - a2 * y2;
        x2 = x1; x1 = in[i];
        y2 = y1; y1 = y;
        out[i] = y;
    }
}

// ---------------------------------------------------------------------------
// DC blocker (1-pole high-pass at 1 Hz)
// y[n] = x[n] - x[n-1] + r * y[n-1],  r = exp(-2*pi*fc/fs)
// ---------------------------------------------------------------------------

void PcgPreprocessor::DcBlocker::set(float fc, float sample_rate) {
    r = std::exp(-2.0f * PI * fc / sample_rate);
    x1 = 0; y1 = 0;
}

void PcgPreprocessor::DcBlocker::reset() {
    x1 = 0; y1 = 0;
}

void PcgPreprocessor::DcBlocker::process(const float* in, float* out, size_t n) {
    float prev_in = x1;
    float prev_y  = y1;
    for (size_t i = 0; i < n; i++) {
        const float y = in[i] - prev_in + r * prev_y;
        out[i] = y;
        prev_in = in[i];
        prev_y  = y;
    }
    x1 = prev_in;
    y1 = prev_y;
}

// ---------------------------------------------------------------------------
// PcgPreprocessor
// ---------------------------------------------------------------------------

PcgPreprocessor::PcgPreprocessor(const Config& cfg) : cfg_(cfg) {
    // DC blocker at 1 Hz
    dc_blocker_.set(1.0f, static_cast<float>(cfg_.sample_rate));

    // Butterworth low-pass at high_hz
    {
        float w0 = 2.0f * PI * cfg_.high_hz / cfg_.sample_rate;
        float bw = std::sin(w0) / (2.0f * 0.707106781f);  // Q = 1/sqrt(2)
        float cos_w0 = std::cos(w0);
        float b0 = (1.0f - cos_w0) / 2.0f;
        float b1 = 1.0f - cos_w0;
        float b2 = (1.0f - cos_w0) / 2.0f;
        float a0 = 1.0f + bw;
        float a1 = -2.0f * cos_w0;
        float a2 = 1.0f - bw;
        lp_.set(b0 / a0, b1 / a0, b2 / a0, a1 / a0, a2 / a0);
    }

    // Butterworth high-pass at low_hz
    {
        float w0 = 2.0f * PI * cfg_.low_hz / cfg_.sample_rate;
        float bw = std::sin(w0) / (2.0f * 0.707106781f);
        float cos_w0 = std::cos(w0);
        float b0 = (1.0f + cos_w0) / 2.0f;
        float b1 = -(1.0f + cos_w0);
        float b2 = (1.0f + cos_w0) / 2.0f;
        float a0 = 1.0f + bw;
        float a1 = -2.0f * cos_w0;
        float a2 = 1.0f - bw;
        hp_.set(b0 / a0, b1 / a0, b2 / a0, a1 / a0, a2 / a0);
    }
}

void PcgPreprocessor::process(const int16_t* in, float* out, size_t count) {
    // Step 1: int16 -> float [-1, 1]
    for (size_t i = 0; i < count; i++) {
        out[i] = static_cast<float>(in[i]) / 32768.0f;
    }

    // Step 2: DC block
    dc_blocker_.process(out, out, count);

    // Step 3: Low-pass
    lp_.process(out, out, count);

    // Step 4: High-pass
    hp_.process(out, out, count);

    // Step 5: RMS normalize
    if (cfg_.normalize && count > 0) {
        float rms = 0.0f;
        for (size_t i = 0; i < count; i++) {
            rms += out[i] * out[i];
        }
        rms = std::sqrt(rms / static_cast<float>(count));

        if (rms > 1e-6f) {
            float gain = cfg_.target_rms / rms;
            for (size_t i = 0; i < count; i++) {
                out[i] *= gain;
            }
        }
    }
}

void PcgPreprocessor::reset() {
    dc_blocker_.reset();
    lp_.reset();
    hp_.reset();
}
