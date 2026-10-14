#pragma once

#include <cstddef>
#include <cstdint>
#include <string>

// RKNN (RV1103 NPU) wrapper for the PCG segmentation-quality network.
//
// Model I/O (see PCGKolect_model/seg/model.py):
//   input  [1, 32, 1, 256] log-filterbank features, float -> quantised to int8 here
//   output [1, 5, 1, 256]  logits (S1, systole, S2, diastole, noise), int8 -> float here
// Uses the zero-copy API, the only one librknnmrt offers on RV1103: NHWC native layout,
// buffers from rknn_create_mem, strides honoured.
class SegModel {
public:
    static constexpr int kBands   = 32;
    static constexpr int kFrames  = 256;
    static constexpr int kClasses = 5;

    SegModel() = default;
    ~SegModel();
    SegModel(const SegModel&) = delete;
    SegModel& operator=(const SegModel&) = delete;

    bool load(const std::string& path, bool verbose = true);
    bool loaded() const { return ctx_ != 0; }

    // feat: [kBands][kFrames] (band-major, i.e. NCHW with H=1).
    // probs: [kFrames][kClasses] softmax probabilities.
    bool run(const float* feat, float* probs);

    // Raw dequantised logits of the last run, [kFrames][kClasses] (for parity tests).
    const float* lastLogits() const { return logits_; }

    // Raw output tensor bytes of the last run (debugging layout issues).
    const void* rawOutput(size_t* bytes) const;

private:
    uint64_t ctx_ = 0;               // rknn_context
    void*    in_mem_  = nullptr;     // rknn_tensor_mem*
    void*    out_mem_ = nullptr;
    void*    in_attr_  = nullptr;    // rknn_tensor_attr*
    void*    out_attr_ = nullptr;
    void*    model_buf_ = nullptr;
    float    logits_[kFrames * kClasses] = {};
};
