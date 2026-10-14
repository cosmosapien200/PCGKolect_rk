// NPU self-test: runs N reference feature windows through the RKNN model on the board and
// writes the dequantised logits, for comparison against PyTorch (PCGKolect_model/seg/check_board.py).
//   npu_selftest model.rknn ref_in.f32 N out_logits.f32
#include "seg_model.hpp"

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <vector>

int main(int argc, char** argv) {
    if (argc < 5) { std::fprintf(stderr, "usage: %s model.rknn ref_in.f32 N out.f32\n", argv[0]); return 1; }
    const int n = std::atoi(argv[3]);
    const size_t per = SegModel::kBands * SegModel::kFrames;
    std::vector<float> in(per * n), out(SegModel::kFrames * SegModel::kClasses * n), probs(SegModel::kFrames * SegModel::kClasses);
    FILE* fp = std::fopen(argv[2], "rb");
    if (!fp || std::fread(in.data(), sizeof(float), in.size(), fp) != in.size()) { std::fprintf(stderr, "bad input\n"); return 1; }
    std::fclose(fp);

    SegModel m;
    if (!m.load(argv[1])) return 1;
    double total_ms = 0;
    for (int i = 0; i < n; i++) {
        auto t0 = std::chrono::steady_clock::now();
        if (!m.run(in.data() + i * per, probs.data())) return 1;
        total_ms += std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
        std::copy(m.lastLogits(), m.lastLogits() + probs.size(), out.begin() + i * probs.size());
        if (i == 0 && std::getenv("NPU_DUMP_RAW")) {
            size_t nb = 0;
            const void* raw = m.rawOutput(&nb);
            FILE* r = std::fopen("raw_out0.bin", "wb");
            std::fwrite(raw, 1, nb, r);
            std::fclose(r);
        }
    }
    std::printf("[npu] %d inferences, mean %.2f ms (incl. quantise + dequantise + softmax)\n", n, total_ms / n);
    fp = std::fopen(argv[4], "wb");
    std::fwrite(out.data(), sizeof(float), out.size(), fp);
    std::fclose(fp);
    return 0;
}
