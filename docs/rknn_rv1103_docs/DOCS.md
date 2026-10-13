# RV1103 RKNN SDK Docs — Index

Three polished, RV1103-focused docs derived from Rockchip V2.3.2 SDK manuals. All non-RV1103 content is excluded or marked unsupported.

---

## Files

| File | Size | What's Inside |
|------|------|--------------|
| `00_RV1103_RKNN_SDK_Quick_Start.md` | ~400 lines | Board setup, env install, Python/C demos, troubleshooting, quick reference table |
| `01_RV1103_RKNN_SDK_User_Guide.md` | ~800 lines | Architecture, env setup, model conversion (config/load/build/export), evaluation, C API, Lite2, advanced features (dynamic shape, hybrid quantization), troubleshooting |
| `02_RV1103_RKNN_API_Reference.md` | ~580 lines | Every API: signature, parameters (type/default/description), return values, RV1103 notes, complete code flows |

---

## How to Look Things Up

**Don't load all files.** Use grep to find what you need:

### Searching by Topic

```bash
# Model conversion flow
grep -n "def \|## 3\." 01_RV1103_RKNN_SDK_User_Guide.md

# Quantization settings
grep -rn "quantized_dtype\|quantized_algorithm\|quantized_method" .

# Runtime / inference
grep -n "init_runtime\|inference\|eval_perf\|eval_memory" .

# RV1103-specific paths
grep -rn "/oem/usr\|librknnmrt\|armhf" .

# API parameters
grep -n "target_platform\|device_id\|perf_debug\|eval_mem" 02_RV1103_RKNN_API_Reference.md

# Supported frameworks
grep -n "ONNX\|PyTorch\|TensorFlow\|load_onnx\|load_pytorch" .
```

### Quick Lookups by Question

| Question | Where to Look |
|----------|--------------|
| "How do I convert a model?" | `01_User_Guide` → Section 3 (config, load, build, export) |
| "What quantization types work?" | `02_API_Reference` → Section 2.2, Quantization Data Types table |
| "How do I run inference on the board?" | `01_User_Guide` → Section 4.1–4.2, or `02_API_Reference` → Section 3.1–3.2 |
| "How do I compile C code?" | `00_Quick_Start` → Section 4.3, or `01_User_Guide` → Section 5 |
| "rknn_server not running?" | `00_Quick_Start` → Section 5.2, or `01_User_Guide` → Section 8.2 |
| "Model too large for memory?" | `00_Quick_Start` → Section 5.3, or `01_User_Guide` → Section 8.2 |
| "Dynamic input shapes?" | `01_User_Guide` → Section 7.1, or `02_API_Reference` → Section 2.2 `dynamic_input` |
| "Accuracy analysis?" | `01_User_Guide` → Section 4.5, or `02_API_Reference` → Section 4.2 |
| "Hybrid quantization?" | `01_User_Guide` → Section 7.2, or `02_API_Reference` → Section 4.1 |
| "What Python version?" | `02_API_Reference` → Section 1 (Python 3.6–3.12) |
| "RV1103 board paths?" | `00_Quick_Start` → Section 6 (Quick Reference table) |
| "Custom operators?" | `01_User_Guide` → Section 7.4, or `02_API_Reference` → Section 4.3 |

### Workflow-Specific Lookups

```bash
# Full conversion flow (Python API)
grep -n "from rknn.api import RKNN" 02_RV1103_RKNN_API_Reference.md

# Full inference flow (Python API)
grep -n "load_rknn\|init_runtime.*rv1103" 02_RV1103_RKNN_API_Reference.md

# C demo compile & deploy
grep -n "build-linux.sh\|adb push.*install" 00_Quick_Start.md

# rknn_server setup
grep -n "restart_rknn\|librknnmrt" .
```

---

## Key RV1103 Facts (Always True)

| Fact | Value |
|------|-------|
| Target platform flag | `rv1103` or `rv1103b` |
| Board architecture | `armhf` (armv7l, 32-bit) |
| Runtime library | `/oem/usr/lib/librknnmrt.so` (NOT `librknnrt.so`) |
| RKNN Server path | `/oem/usr/bin/rknn_server` |
| GCC cross-compiler | `arm-rockchip830-linux-uclibcgnueabihf` |
| Recommended Python | 3.8 |
| Supported quantization | `w8a8`, `w16a16i`, `w16a16i_dfp` |
| Encryption support | **No** (RV1103 does not support model encryption) |
| Multi-core NPU | **No** (single core only) |
| Dynamic shape req. | RKNN Server & Runtime >= 1.5.0 |
| Recommended rknn_server | >= 2.1.0 (small memory platform) |
