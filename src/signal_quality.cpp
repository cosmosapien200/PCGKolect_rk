#include "signal_quality.hpp"
#include "sqi_model_params.h"

#include <algorithm>
#include <cmath>
#include <complex>
#include <cstdlib>

// Every step here mirrors PCGKolect_model/sqi_features.py and sqi_frontend.py.
// Keep them in lock-step; PCGKolect_model/tests/test_parity.py checks it.
// Do not build with -ffast-math (it reorders the sums the parity test relies on).

static_assert(sqi_params::kNumFeatures == SignalQuality::kNumFeatures,
              "sqi_model_params.h does not match SignalQuality");

namespace {

constexpr double kPi      = 3.14159265358979323846;
constexpr int    kFs      = 2000;
constexpr int    kCtx     = 8000;     // 4 s
constexpr int    kC0      = 2000;     // centre 2 s = [kC0, kC1)
constexpr int    kC1      = 6000;
constexpr int    kNfft    = 256;
constexpr int    kWelchHop = 128;
constexpr int    kEnvBlk  = 40;       // 50 Hz envelope
constexpr int    kEnvLen  = kCtx / kEnvBlk;
constexpr int    kLagLo   = 15;       // 0.3 s
constexpr int    kLagHi   = 75;       // 1.5 s (exclusive)
constexpr double kBands[5][2] = {{20, 60}, {60, 150}, {150, 300}, {300, 600}, {600, 1001}};

double median(std::vector<double> v) {
    const size_t n = v.size();
    std::nth_element(v.begin(), v.begin() + n / 2, v.end());
    double hi = v[n / 2];
    if (n % 2) return hi;
    double lo = *std::max_element(v.begin(), v.begin() + n / 2);
    return 0.5 * (lo + hi);
}

// Block-energy features over `nblk` equal blocks: std(log e), log(max/median), frac(e > 10*median)
void blockFeatures(const double* z, int n, int nblk, double& cv, double& maxmed, double& burst) {
    const int len = n / nblk;
    std::vector<double> e(nblk), le(nblk);
    for (int b = 0; b < nblk; b++) {
        double s = 0;
        for (int i = 0; i < len; i++) s += z[b * len + i] * z[b * len + i];
        e[b]  = s / len + 1e-12;
        le[b] = std::log(e[b]);
    }
    double m = 0;
    for (double v : le) m += v;
    m /= nblk;
    double var = 0;
    for (double v : le) var += (v - m) * (v - m);
    cv = std::sqrt(var / nblk);
    const double med = median(e);
    maxmed = std::log(*std::max_element(e.begin(), e.end()) / med);
    int cnt = 0;
    for (double v : e) cnt += v > 10 * med;
    burst = static_cast<double>(cnt) / nblk;
}

// In-place iterative radix-2 FFT.
void fft(std::complex<double>* a, int n) {
    for (int i = 1, j = 0; i < n; i++) {
        int bit = n >> 1;
        for (; j & bit; bit >>= 1) j ^= bit;
        j ^= bit;
        if (i < j) std::swap(a[i], a[j]);
    }
    for (int len = 2; len <= n; len <<= 1) {
        const double ang = -2 * kPi / len;
        for (int i = 0; i < n; i += len) {
            for (int k = 0; k < len / 2; k++) {
                const std::complex<double> w(std::cos(ang * k), std::sin(ang * k));
                const std::complex<double> u = a[i + k], v = a[i + k + len / 2] * w;
                a[i + k] = u + v;
                a[i + k + len / 2] = u - v;
            }
        }
    }
}

} // namespace

// ---------------------------------------------------------------------------
// Features of one 4 s @ 2 kHz buffer (see sqi_features.py for definitions)
// ---------------------------------------------------------------------------

void SignalQuality::computeFeatures(const double* xin, double* f) {
    std::vector<double> x(xin, xin + kCtx), z(kCtx);
    double mean = 0;
    for (double v : x) mean += v;
    mean /= kCtx;
    double ss = 0;
    for (double& v : x) { v -= mean; ss += v * v; }
    const double rms4 = std::sqrt(ss / kCtx) + 1e-8;
    for (int i = 0; i < kCtx; i++) z[i] = x[i] / rms4;

    // --- centre 2 s ---
    const int nc = kC1 - kC0;
    std::vector<double> zc(x.begin() + kC0, x.begin() + kC1);
    double mc = 0;
    for (double v : zc) mc += v;
    mc /= nc;
    double sc = 0;
    for (double& v : zc) { v -= mc; sc += v * v; }
    const double rmsc = std::sqrt(sc / nc) + 1e-8;
    double k4 = 0, crest = 0;
    for (double& v : zc) {
        v /= rmsc;
        k4 += v * v * v * v;
        crest = std::max(crest, std::fabs(v));
    }
    int zc_count = 0;
    for (int i = 1; i < nc; i++) zc_count += (zc[i] >= 0) != (zc[i - 1] >= 0);

    f[0] = std::log(k4 / nc + 1e-12);
    f[1] = crest;
    f[2] = static_cast<double>(zc_count) / (nc - 1);
    blockFeatures(zc.data(), nc, 20, f[3], f[4], f[5]);

    // Welch PSD: 256-pt symmetric Hann, hop 128
    static double hann[kNfft];
    static bool hann_init = false;
    if (!hann_init) {
        for (int i = 0; i < kNfft; i++) hann[i] = 0.5 - 0.5 * std::cos(2 * kPi * i / (kNfft - 1));
        hann_init = true;
    }
    double P[kNfft / 2 + 1] = {};
    int nseg = 0;
    std::complex<double> buf[kNfft];
    for (int s = 0; s + kNfft <= nc; s += kWelchHop, nseg++) {
        for (int i = 0; i < kNfft; i++) buf[i] = zc[s + i] * hann[i];
        fft(buf, kNfft);
        for (int k = 0; k <= kNfft / 2; k++) P[k] += std::norm(buf[k]);
    }
    double tot = 0, lsum = 0, fsum = 0, band[5] = {};
    int nsel = 0;
    for (int k = 0; k <= kNfft / 2; k++) {
        const double fk = k * static_cast<double>(kFs) / kNfft;
        if (fk < 20) continue;
        const double p = P[k] / nseg + 1e-12;
        tot += p; lsum += std::log(p); fsum += p * fk; nsel++;
        for (int b = 0; b < 5; b++)
            if (fk >= kBands[b][0] && fk < kBands[b][1]) band[b] += p;
    }
    f[6] = lsum / nsel - std::log(tot / nsel);
    f[7] = fsum / tot / 1000.0;
    for (int b = 0; b < 5; b++) f[8 + b] = std::log(band[b] / tot);

    // --- 4 s context ---
    blockFeatures(z.data(), kCtx, 40, f[13], f[14], f[15]);

    double env[kEnvLen];
    double em = 0;
    for (int j = 0; j < kEnvLen; j++) {
        double s = 0;
        for (int i = 0; i < kEnvBlk; i++) s += std::fabs(z[j * kEnvBlk + i]);
        env[j] = s / kEnvBlk;
        em += env[j];
    }
    em /= kEnvLen;
    double en = 0;
    for (double& v : env) { v -= em; en += v * v; }
    en = std::sqrt(en) + 1e-8;
    for (double& v : env) v /= en;

    double best = -1e300;
    int best_lag = kLagLo;
    for (int L = kLagLo; L < kLagHi; L++) {
        double s = 0;
        for (int i = 0; i < kEnvLen - L; i++) s += env[i] * env[i + L];
        s /= (1.0 - static_cast<double>(L) / kEnvLen);
        if (s > best) { best = s; best_lag = L; }
    }
    f[16] = best;
    f[17] = static_cast<double>(best_lag) / (static_cast<double>(kFs) / kEnvBlk);

    double psum = 0, p[kEnvLen];
    for (int j = 0; j < kEnvLen; j++) { p[j] = env[j] * env[j] + 1e-12; psum += p[j]; }
    double H = 0;
    for (int j = 0; j < kEnvLen; j++) { const double q = p[j] / psum; H -= q * std::log(q); }
    f[18] = H / std::log(static_cast<double>(kEnvLen));

    f[19] = std::log(rmsc);   // log_rms (gates only)
}

double SignalQuality::scoreFeatures(const double* f) {
    double logit = sqi_params::kIntercept;
    for (int i = 0; i < kNumFeatures; i++)
        logit += sqi_params::kCoef[i] * (f[i] - sqi_params::kMean[i]) / sqi_params::kScale[i];
    return 1.0 / (1.0 + std::exp(-logit));
}

// ---------------------------------------------------------------------------
// Streaming front-end
// ---------------------------------------------------------------------------

SignalQuality::SignalQuality() { reset(); }

void SignalQuality::reset() {
    std::fill(std::begin(fir_hist_), std::end(fir_hist_), 0.0);
    std::fill(std::begin(ring_), std::end(ring_), 0.0);
    std::fill(std::begin(clip_ring_), std::end(clip_ring_), 0);
    fir_pos_ = phase_ = clip_acc_ = ring_pos_ = since_step_ = 0;
    hp_z1_ = hp_z2_ = pending_ = 0;
    total_2k_ = 0;
}

size_t SignalQuality::push(const int16_t* pcm, size_t count, std::vector<Result>& out) {
    size_t produced = 0;
    for (size_t n = 0; n < count; n++) {
        const int s = pcm[n];
        fir_hist_[fir_pos_] = s / 32768.0;
        clip_acc_ += std::abs(s) >= sqi_params::kClipLevel;

        if (phase_ == 0) {
            // y[n] = sum_k h[k] x[n-k]  (scipy lfilter, zero initial state)
            double y = 0;
            int idx = fir_pos_;
            for (int k = 0; k < kTaps; k++) {
                y += sqi_params::kDecimTaps[k] * fir_hist_[idx];
                idx = idx == 0 ? kTaps - 1 : idx - 1;
            }
            pending_ = y;
        }
        fir_pos_ = (fir_pos_ + 1) % kTaps;

        if (phase_ == kDecim - 1) {
            pushDecimated(pending_, clip_acc_, out, produced);
            clip_acc_ = 0;
        }
        phase_ = (phase_ + 1) % kDecim;
    }
    return produced;
}

void SignalQuality::pushDecimated(double x, int clipped, std::vector<Result>& out, size_t& produced) {
    using namespace sqi_params;
    // Biquad, transposed direct form II (identical to scipy sosfilt)
    const double y = kHpSos[0] * x + hp_z1_;
    hp_z1_ = kHpSos[1] * x - kHpSos[4] * y + hp_z2_;
    hp_z2_ = kHpSos[2] * x - kHpSos[5] * y;

    ring_[ring_pos_] = y;
    clip_ring_[ring_pos_] = static_cast<uint8_t>(clipped);
    ring_pos_ = (ring_pos_ + 1) % kCtx;
    total_2k_++;

    if (total_2k_ >= kCtx && (total_2k_ - kCtx) % kStep == 0) {
        scoreNow(out);
        produced++;
    }
}

void SignalQuality::scoreNow(std::vector<Result>& out) {
    using namespace sqi_params;
    double lin[kCtx];
    int clips = 0;
    for (int i = 0; i < kCtx; i++) {
        const int r = (ring_pos_ + i) % kCtx;   // oldest first
        lin[i] = ring_[r];
        if (i >= kC0 && i < kC1) clips += clip_ring_[r];
    }

    Result res;
    computeFeatures(lin, res.features);
    res.q          = scoreFeatures(res.features);
    res.t_start_s  = static_cast<double>(total_2k_ - kCtx + kFs) / kFs;
    res.t_end_s    = static_cast<double>(total_2k_ - kFs) / kFs;
    res.clip_frac  = static_cast<double>(clips) / ((kC1 - kC0) * kDecim);
    res.level_dbfs = 20.0 * std::log10(std::exp(res.features[kNumFeatures]) + 1e-12);

    if (res.clip_frac > kClipFracMax)            { res.verdict = BAD;      res.reason = CLIPPING; }
    else if (res.level_dbfs < kLevelDbfsMin)     { res.verdict = BAD;      res.reason = NO_CONTACT; }
    else if (res.level_dbfs > kLevelDbfsMax)     { res.verdict = BAD;      res.reason = TOO_LOUD; }
    else if (res.q >= kThreshGood)               { res.verdict = GOOD;     res.reason = OK; }
    else if (res.q >= kThreshBad)                { res.verdict = MARGINAL; res.reason = NOISY; }
    else                                         { res.verdict = BAD;      res.reason = NOISY; }

    const double per_peak = res.features[16], per_lag = res.features[17];
    res.hr_bpm = (res.verdict == GOOD && per_peak >= kHrMinPeriodicity) ? 60.0 / per_lag : 0.0;
    out.push_back(res);
}

const char* SignalQuality::verdictName(Verdict v) {
    static const char* n[] = {"GOOD", "MARGINAL", "BAD"};
    return n[v];
}

const char* SignalQuality::reasonName(Reason r) {
    static const char* n[] = {"OK", "NOISY", "NO_CONTACT", "TOO_LOUD", "CLIPPING"};
    return n[r];
}

// ---------------------------------------------------------------------------
// QualityTracker
// ---------------------------------------------------------------------------

void QualityTracker::add(const SignalQuality::Result& r, double step_s) {
    s_.scored_s += step_s;
    q_sum_ += r.q;
    switch (r.verdict) {
    case SignalQuality::GOOD:
        s_.good_s += step_s;
        s_.usable_beats += r.hr_bpm / 60.0 * step_s;
        s_.current_run_s += step_s;
        s_.longest_run_s = std::max(s_.longest_run_s, s_.current_run_s);
        break;
    case SignalQuality::MARGINAL:
        s_.marginal_s += step_s;
        s_.current_run_s = 0;
        break;
    default:
        s_.bad_s += step_s;
        s_.current_run_s = 0;
        break;
    }
    s_.usable_ratio = s_.good_s / s_.scored_s;
    s_.mean_q = q_sum_ / (s_.scored_s / step_s);
}
