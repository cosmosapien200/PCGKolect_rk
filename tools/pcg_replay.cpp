// Board-side replay: run a recorded int16 @ 8 kHz file through the exact firmware pipeline
// (SegQuality + NPU SegModel, plus v1 SignalQuality) and print the beats.
//   pcg_replay model.rknn recording.raw
#include "seg_model.hpp"
#include "seg_quality.hpp"
#include "signal_quality.hpp"

#include <cstdio>
#include <vector>

int main(int argc, char** argv) {
    if (argc < 3) { std::fprintf(stderr, "usage: %s model.rknn pcm16_8k.raw\n", argv[0]); return 1; }
    FILE* fp = std::fopen(argv[2], "rb");
    if (!fp) return 1;
    std::vector<int16_t> pcm;
    int16_t buf[800];
    size_t n;
    while ((n = std::fread(buf, 2, 800, fp)) > 0) pcm.insert(pcm.end(), buf, buf + n);
    std::fclose(fp);

    SegModel m;
    if (!m.load(argv[1], false)) return 1;
    SegQuality seg([&](const float* f, float* p) { return m.run(f, p); });
    BeatTracker tr;
    std::vector<SegQuality::Beat> bs;
    double ms = 0;
    int runs = 0;
    for (size_t i = 0; i < pcm.size(); i += 800) {
        runs += seg.push(pcm.data() + i, std::min<size_t>(800, pcm.size() - i), bs);
        ms += seg.lastInferMs();
    }
    for (auto& b : bs) {
        tr.add(b);
        std::printf("beat %ld %ld %.4f %d\n", b.start_frame, b.end_frame, b.conf, b.usable ? 1 : 0);
    }
    const auto& s = tr.summary();
    std::printf("#summary beats=%d usable=%d longest_run=%d hr=%.1f inferences=%d\n",
                s.beats, s.usable_beats, s.longest_run, s.hr_bpm, runs);
    return 0;
}
