#include "seg_model.hpp"

#include "rknn_api.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>

namespace {

void dumpAttr(const char* tag, const rknn_tensor_attr& a) {
    std::printf("[npu] %s: dims=[", tag);
    for (uint32_t i = 0; i < a.n_dims; i++) std::printf("%s%u", i ? "," : "", a.dims[i]);
    std::printf("] fmt=%s type=%s size=%u size_with_stride=%u w_stride=%u zp=%d scale=%g\n",
                get_format_string(a.fmt), get_type_string(a.type), a.size, a.size_with_stride,
                a.w_stride, a.zp, a.scale);
}

} // namespace

SegModel::~SegModel() {
    rknn_context ctx = static_cast<rknn_context>(ctx_);
    if (in_mem_)  rknn_destroy_mem(ctx, static_cast<rknn_tensor_mem*>(in_mem_));
    if (out_mem_) rknn_destroy_mem(ctx, static_cast<rknn_tensor_mem*>(out_mem_));
    if (ctx_)     rknn_destroy(ctx);
    delete static_cast<rknn_tensor_attr*>(in_attr_);
    delete static_cast<rknn_tensor_attr*>(out_attr_);
    std::free(model_buf_);
}

bool SegModel::load(const std::string& path, bool verbose) {
    FILE* fp = std::fopen(path.c_str(), "rb");
    if (!fp) { std::fprintf(stderr, "[npu] cannot open %s\n", path.c_str()); return false; }
    std::fseek(fp, 0, SEEK_END);
    const long size = std::ftell(fp);
    std::fseek(fp, 0, SEEK_SET);
    model_buf_ = std::malloc(size);
    if (std::fread(model_buf_, 1, size, fp) != static_cast<size_t>(size)) { std::fclose(fp); return false; }
    std::fclose(fp);

    rknn_context ctx = 0;
    int ret = rknn_init(&ctx, model_buf_, static_cast<uint32_t>(size), 0, nullptr);
    if (ret < 0) { std::fprintf(stderr, "[npu] rknn_init failed: %d\n", ret); return false; }
    ctx_ = ctx;

    rknn_input_output_num io{};
    rknn_query(ctx, RKNN_QUERY_IN_OUT_NUM, &io, sizeof(io));
    if (io.n_input != 1 || io.n_output != 1) {
        std::fprintf(stderr, "[npu] unexpected io count %u/%u\n", io.n_input, io.n_output);
        return false;
    }

    auto* in = new rknn_tensor_attr{};
    auto* out = new rknn_tensor_attr{};
    in_attr_ = in; out_attr_ = out;
    in->index = 0; out->index = 0;
    if (rknn_query(ctx, RKNN_QUERY_NATIVE_INPUT_ATTR, in, sizeof(*in)) < 0 ||
        rknn_query(ctx, RKNN_QUERY_NATIVE_NHWC_OUTPUT_ATTR, out, sizeof(*out)) < 0) {
        std::fprintf(stderr, "[npu] rknn_query attrs failed\n");
        return false;
    }
    // Features are quantised on the CPU with the model's zp/scale and written straight into the
    // NPU's native layout (pass-through). On RV1103 the native input is NC1HWC2 for C=32;
    // the runtime cannot convert int8 NHWC -> NC1HWC2 itself.
    in->type = RKNN_TENSOR_INT8;
    if (in->fmt == RKNN_TENSOR_NC1HWC2) in->pass_through = 1;
    else in->fmt = RKNN_TENSOR_NHWC;
    if (verbose) { dumpAttr("input ", *in); dumpAttr("output", *out); }

    auto* im = rknn_create_mem(ctx, in->size_with_stride);
    auto* om = rknn_create_mem(ctx, out->size_with_stride);
    in_mem_ = im; out_mem_ = om;
    if (!im || !om ||
        rknn_set_io_mem(ctx, im, in) < 0 || rknn_set_io_mem(ctx, om, out) < 0) {
        std::fprintf(stderr, "[npu] rknn_create_mem / rknn_set_io_mem failed\n");
        return false;
    }
    return true;
}

bool SegModel::run(const float* feat, float* probs) {
    auto* in = static_cast<rknn_tensor_attr*>(in_attr_);
    auto* out = static_cast<rknn_tensor_attr*>(out_attr_);
    auto* im = static_cast<rknn_tensor_mem*>(in_mem_);
    auto* om = static_cast<rknn_tensor_mem*>(out_mem_);

    auto* dst = static_cast<int8_t*>(im->virt_addr);
    std::memset(dst, 0, in->size_with_stride);
    const float inv = 1.0f / in->scale;
    const bool nc1hwc2 = in->fmt == RKNN_TENSOR_NC1HWC2;
    const int c2 = nc1hwc2 ? static_cast<int>(in->dims[4]) : kBands;      // dims [1, C1, H, W, C2]
    const int wst = in->w_stride ? static_cast<int>(in->w_stride) : kFrames;
    for (int w = 0; w < kFrames; w++) {
        for (int c = 0; c < kBands; c++) {
            const int q = static_cast<int>(std::lround(feat[c * kFrames + w] * inv)) + in->zp;
            const size_t idx = nc1hwc2 ? (static_cast<size_t>(c / c2) * wst + w) * c2 + c % c2   // NC1HWC2
                                       : static_cast<size_t>(w) * kBands + c;                     // NHWC
            dst[idx] = static_cast<int8_t>(std::max(-128, std::min(127, q)));
        }
    }
    rknn_mem_sync(static_cast<rknn_context>(ctx_), im, RKNN_MEMORY_SYNC_TO_DEVICE);

    if (rknn_run(static_cast<rknn_context>(ctx_), nullptr) < 0) {
        std::fprintf(stderr, "[npu] rknn_run failed\n");
        return false;
    }
    rknn_mem_sync(static_cast<rknn_context>(ctx_), om, RKNN_MEMORY_SYNC_FROM_DEVICE);

    const auto* src = static_cast<const int8_t*>(om->virt_addr);
    if (out->fmt == RKNN_TENSOR_NHWC) {
        // dims [1, H=1, W=256, C=5]. On RV1103 the data is dense (out->size bytes); the larger
        // size_with_stride is allocation padding, not a per-pixel channel stride.
        const int cstride = static_cast<int>(out->size / kFrames);
        for (int w = 0; w < kFrames; w++)
            for (int c = 0; c < kClasses; c++)
                logits_[w * kClasses + c] = (src[w * cstride + c] - out->zp) * out->scale;
    } else if (out->fmt == RKNN_TENSOR_NC1HWC2) {
        // dims [1, C1, H, W, C2]
        const int c2 = static_cast<int>(out->dims[4]);
        for (int c = 0; c < kClasses; c++)
            for (int w = 0; w < kFrames; w++)
                logits_[w * kClasses + c] =
                    (src[((c / c2) * kFrames + w) * c2 + (c % c2)] - out->zp) * out->scale;
    } else {  // NCHW
        for (int c = 0; c < kClasses; c++)
            for (int w = 0; w < kFrames; w++)
                logits_[w * kClasses + c] = (src[c * kFrames + w] - out->zp) * out->scale;
    }

    for (int w = 0; w < kFrames; w++) {
        const float* l = logits_ + w * kClasses;
        float m = l[0];
        for (int c = 1; c < kClasses; c++) m = std::max(m, l[c]);
        float s = 0;
        for (int c = 0; c < kClasses; c++) { probs[w * kClasses + c] = std::exp(l[c] - m); s += probs[w * kClasses + c]; }
        for (int c = 0; c < kClasses; c++) probs[w * kClasses + c] /= s;
    }
    return true;
}

const void* SegModel::rawOutput(size_t* bytes) const {
    auto* om = static_cast<rknn_tensor_mem*>(out_mem_);
    if (bytes) *bytes = om ? om->size : 0;
    return om ? om->virt_addr : nullptr;
}
