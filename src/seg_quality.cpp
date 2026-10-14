#include "seg_quality.hpp"
#include "seg_params.h"
#include "sqi_model_params.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <complex>
#include <limits>

// Mirrors PCGKolect_model/seg/model.py (Frontend) and seg/decode.py.
// PCGKolect_model/tests/test_seg_parity.py checks both against Python.

using namespace seg_params;

namespace {

constexpr int kStepSamples = 2000;                 // 1 s @ 2 kHz between inferences
constexpr int kStepFrames  = kStepSamples / kHop;  // 50
constexpr int kRightCtx    = 64;                   // frames of future context required (1.28 s)
constexpr int kHistFrames  = 1000;                 // 20 s posterior history for decoding
constexpr int kStableMargin = 25;                  // beats must end >= 0.5 s before the newest frame
constexpr double kPi = 3.14159265358979323846;

void fft(std::complex<double>* a, int n) {
    for (int i = 1, j = 0; i < n; i++) {
        int bit = n >> 1;
        for (; j & bit; bit >>= 1) j ^= bit;
        j ^= bit;
        if (i < j) std::swap(a[i], a[j]);
    }
    for (int len = 2; len <= n; len <<= 1) {
        const double ang = -2 * kPi / len;
        for (int i = 0; i < n; i += len)
            for (int k = 0; k < len / 2; k++) {
                const std::complex<double> w(std::cos(ang * k), std::sin(ang * k));
                const std::complex<double> u = a[i + k], v = a[i + k + len / 2] * w;
                a[i + k] = u + v;
                a[i + k + len / 2] = u - v;
            }
    }
}

// ---- expanded HMM (seg/decode.py::_build) ----
struct Hmm {
    std::vector<int> states;           // class of each expanded state
    int first[5], last[5];
    std::vector<std::vector<std::pair<int, double>>> preds;   // to -> [(from, logA)]
    Hmm() {
        static const int next[4] = {1, 2, 3, 0};
        for (int c = 0; c < 5; c++) {
            first[c] = static_cast<int>(states.size());
            for (int i = 0; i < kMinDur[c]; i++) states.push_back(c);
            last[c] = static_cast<int>(states.size()) - 1;
        }
        const int n = static_cast<int>(states.size());
        std::vector<std::vector<double>> A(n, std::vector<double>(n, -std::numeric_limits<double>::infinity()));
        for (int c = 0; c < 5; c++) {
            for (int i = first[c]; i < last[c]; i++) A[i][i + 1] = 0.0;
            const double stay = 1 - 1 / std::max(kMeanDur[c] - kMinDur[c] + 1, 1.0001);
            const int l = last[c];
            A[l][l] = std::log(stay);
            if (c < 4) {
                A[l][first[next[c]]] = std::log((1 - stay) * (1 - kPToNoise));
                A[l][first[4]] = std::log((1 - stay) * kPToNoise);
            } else {
                for (int k = 0; k < 4; k++) A[l][first[k]] = std::log((1 - stay) / 4);
            }
        }
        preds.resize(n);
        for (int j = 0; j < n; j++)
            for (int i = 0; i < n; i++)
                if (std::isfinite(A[i][j])) preds[j].push_back({i, A[i][j]});
    }
};

const Hmm& hmm() {
    static const Hmm h;
    return h;
}

double median(std::vector<double> v) {
    const size_t n = v.size();
    std::sort(v.begin(), v.end());
    return n % 2 ? v[n / 2] : 0.5 * (v[n / 2 - 1] + v[n / 2]);
}

} // namespace

// ---------------------------------------------------------------------------
// Front-end
// ---------------------------------------------------------------------------

void SegQuality::features(const float* x, float* feat) {
    std::complex<double> buf[kNfft];
    double P[kNfft / 2 + 1];
    double sum = 0;
    for (int t = 0; t < kFrames; t++) {
        for (int i = 0; i < kNfft; i++) buf[i] = static_cast<double>(x[t * kHop + i]) * kHann[i];
        fft(buf, kNfft);
        for (int k = 0; k <= kNfft / 2; k++) P[k] = std::norm(buf[k]);
        for (int b = 0; b < kBands; b++) {
            double e = 0;
            for (int k = 0; k <= kNfft / 2; k++) e += kFb[b][k] * P[k];
            const double m = std::log(e + 1e-10);
            feat[b * kFrames + t] = static_cast<float>(m);
            sum += m;
        }
    }
    const double mean = sum / (kBands * kFrames);
    for (int i = 0; i < kBands * kFrames; i++)
        feat[i] = static_cast<float>((feat[i] - mean) / kFeatScale);
}

// ---------------------------------------------------------------------------
// Decoder
// ---------------------------------------------------------------------------

void SegQuality::viterbi(const float* P, int T, std::vector<int>& cls) {
    const Hmm& h = hmm();
    const int n = static_cast<int>(h.states.size());
    const double NEG = -std::numeric_limits<double>::infinity();
    std::vector<double> delta(n, NEG), nd(n);
    std::vector<int> back(static_cast<size_t>(T) * n, 0);
    auto E = [&](int t, int s) { return std::log(static_cast<double>(P[t * 5 + h.states[s]]) + kEps); };
    for (int c = 0; c < 5; c++) delta[h.first[c]] = E(0, h.first[c]);
    for (int t = 1; t < T; t++) {
        for (int j = 0; j < n; j++) {
            double best = NEG;
            int arg = 0;
            for (const auto& pa : h.preds[j]) {
                const double v = delta[pa.first] + pa.second;
                if (v > best) { best = v; arg = pa.first; }
            }
            back[static_cast<size_t>(t) * n + j] = arg;
            nd[j] = best + E(t, j);
        }
        delta.swap(nd);
    }
    int s = 0;
    for (int j = 1; j < n; j++) if (delta[j] > delta[s]) s = j;
    cls.assign(T, 0);
    for (int t = T - 1; t >= 0; t--) {
        cls[t] = h.states[s];
        if (t > 0) s = back[static_cast<size_t>(t) * n + s];
    }
}

void SegQuality::beats(const std::vector<int>& cls, const float* P, int T, std::vector<Beat>& out) {
    struct Run { int c, s, e; };
    std::vector<Run> runs;
    for (int t = 0; t < T; t++) {
        if (runs.empty() || runs.back().c != cls[t]) runs.push_back({cls[t], t, t + 1});
        else runs.back().e = t + 1;
    }
    std::vector<Beat> bs;
    for (size_t i = 0; i + 4 < runs.size(); i++) {
        if (runs[i].c != 0 || runs[i + 1].c != 1 || runs[i + 2].c != 2 || runs[i + 3].c != 3 || runs[i + 4].c != 0)
            continue;
        if (runs[i].s == 0) continue;                  // first S1 may be truncated
        const int s = runs[i].s, e = runs[i + 4].s;
        double conf = 0, pn = 0;
        for (int t = s; t < e; t++) { conf += P[t * 5 + cls[t]]; pn += P[t * 5 + 4]; }
        Beat b;
        b.start_frame = s; b.end_frame = e;
        b.dur_s = static_cast<double>(e - s) / kFps;
        b.conf = conf / (e - s);
        b.pnoise = pn / (e - s);
        bs.push_back(b);
    }
    for (size_t j = 0; j < bs.size(); j++) {
        std::vector<double> nb;
        for (size_t k = (j >= 3 ? j - 3 : 0); k < std::min(bs.size(), j + 4); k++) nb.push_back(bs[k].dur_s);
        const double med = median(nb);
        Beat& b = bs[j];
        b.rhythm_dev = std::fabs(b.dur_s - med) / med;
        b.ok_dur = b.dur_s >= kBeatMinS && b.dur_s <= kBeatMaxS;
        b.ok_conf = b.conf >= kBeatConfMin;
        b.ok_rhythm = b.rhythm_dev <= kRhythmTol || nb.size() < 3;
        b.score = b.conf * ((b.ok_dur && b.ok_rhythm) ? 1.0 : 0.5);
        b.usable = b.ok_dur && b.ok_conf && b.ok_rhythm;
        out.push_back(b);
    }
}

// ---------------------------------------------------------------------------
// Streaming
// ---------------------------------------------------------------------------

SegQuality::SegQuality(InferFn infer) : infer_(std::move(infer)), ring_(kSamples, 0.0f) {}

int SegQuality::push(const int16_t* pcm, size_t count, std::vector<Beat>& out) {
    using namespace sqi_params;
    int runs = 0;
    for (size_t n = 0; n < count; n++) {
        fir_hist_[fir_pos_] = pcm[n] / 32768.0;
        if (phase_ == 0) {
            double y = 0;
            int idx = fir_pos_;
            for (int k = 0; k < 63; k++) {
                y += kDecimTaps[k] * fir_hist_[idx];
                idx = idx == 0 ? 62 : idx - 1;
            }
            const double h = kHpSos[0] * y + hp_z1_;
            hp_z1_ = kHpSos[1] * y - kHpSos[4] * h + hp_z2_;
            hp_z2_ = kHpSos[2] * y - kHpSos[5] * h;
            ring_[ring_pos_] = static_cast<float>(h);
            ring_pos_ = (ring_pos_ + 1) % kSamples;
            total_2k_++;
            if (total_2k_ >= kSamples && (total_2k_ - kSamples) % kStepSamples == 0) {
                step(out);
                runs++;
            }
        }
        fir_pos_ = (fir_pos_ + 1) % 63;
        phase_ = (phase_ + 1) % 4;
    }
    return runs;
}

void SegQuality::step(std::vector<Beat>& out) {
    std::vector<float> x(kSamples), feat(kBands * kFrames), probs(kFrames * kClasses);
    for (int i = 0; i < kSamples; i++) x[i] = ring_[(ring_pos_ + i) % kSamples];
    const auto t0 = std::chrono::steady_clock::now();
    features(x.data(), feat.data());
    const bool ok = infer_(feat.data(), probs.data());
    last_infer_ms_ = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
    if (!ok) return;

    const long g0 = (total_2k_ - kSamples) / kHop;                  // absolute frame of probs[0]
    const int a = next_frame_ == 0 ? 0 : kFrames - kRightCtx - kStepFrames;
    double nsum = 0, csum = 0;
    int cnt = 0;
    for (int i = a; i < kFrames - kRightCtx; i++) {
        if (g0 + i != next_frame_) continue;
        const float* p = &probs[i * kClasses];
        for (int c = 0; c < kClasses; c++) hist_.push_back(p[c]);
        next_frame_++;
        nsum += p[kNoise];
        csum += *std::max_element(p, p + 4);
        cnt++;
    }
    if (cnt) { recent_noise_ = nsum / cnt; recent_conf_ = csum / cnt; }
    while (static_cast<long>(hist_.size()) > kHistFrames * kClasses) {
        for (int c = 0; c < kClasses; c++) hist_.pop_front();
        hist_start_++;
    }
    decode(out, false);
}

void SegQuality::decode(std::vector<Beat>& out, bool flush) {
    const int T = static_cast<int>(hist_.size() / kClasses);
    if (T < 2 * kFps) return;
    std::vector<float> P(hist_.begin(), hist_.end());
    std::vector<int> cls;
    viterbi(P.data(), T, cls);
    std::vector<Beat> bs;
    beats(cls, P.data(), T, bs);
    const long limit = flush ? next_frame_ : next_frame_ - kStableMargin;
    for (Beat& b : bs) {
        const long s = hist_start_ + b.start_frame, e = hist_start_ + b.end_frame;
        if (e > limit || e <= last_emitted_end_ + 5 || s < last_emitted_end_ - 5) continue;
        b.start_frame = s;
        b.end_frame = e;
        b.t_start_s = static_cast<double>(s) / kFps;
        b.t_end_s = static_cast<double>(e) / kFps;
        out.push_back(b);
        last_emitted_end_ = e;
    }
}

// ---------------------------------------------------------------------------

void BeatTracker::add(const SegQuality::Beat& b) {
    s_.beats++;
    // Noise produces no beats at all, so a gap between consecutive beats also breaks a run.
    if (last_end_ >= 0 && b.start_frame - last_end_ > 5) s_.run = 0;
    last_end_ = b.end_frame;
    if (b.usable) {
        s_.usable_beats++;
        s_.usable_s += b.dur_s;
        s_.run++;
        s_.longest_run = std::max(s_.longest_run, s_.run);
        recent_.push_back(b.dur_s);
        if (recent_.size() > 8) recent_.pop_front();
        std::vector<double> v(recent_.begin(), recent_.end());
        s_.hr_bpm = 60.0 / median(v);
    } else {
        s_.run = 0;
    }
    s_.usable_ratio = static_cast<double>(s_.usable_beats) / s_.beats;
}
