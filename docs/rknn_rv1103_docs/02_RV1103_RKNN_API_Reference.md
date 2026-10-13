# RV1103 RKNN-Toolkit2 API Reference

> **Version:** V2.3.2 | **Release Date:** 2025-04-01
>
> RV1103-specific API reference. Only RV1103-relevant APIs and parameters are included.

---

## 1. Overview

RKNN-Toolkit2 is a PC-side Python SDK for model conversion, evaluation, and board-side debugging.

**Supported Python versions:** 3.6, 3.7, 3.8, 3.10, 3.11, 3.12
**Supported OS:** Ubuntu 18.04 / 20.04 / 22.04 / 24.04 (x64)

### Supported Deep Learning Frameworks

| Framework | Supported Versions |
|-----------|-------------------|
| ONNX | 1.7.0 – 1.17.0 |
| PyTorch | 1.6.0 – 2.4.0 (torchscript format) |
| TensorFlow | 1.12.0 – 2.14.0 |
| TensorFlow Lite | Schema version = 3 |
| Caffe | 1.0 |
| DarkNet | Commit 810d7f7 |

---

## 2. Core APIs

### 2.1 RKNN Initialization and Release

```python
from rknn.api import RKNN

# Initialize
rknn = RKNN(verbose=False, verbose_file=None)

# ... perform operations ...

# Release all resources
rknn.release()
```

| Parameter | Type | Default | Description |
|-----------|------|---------|-------------|
| `verbose` | bool | `False` | Print detailed logs to screen |
| `verbose_file` | str | `None` | Write logs to file (requires `verbose=True`) |

**Return:** None

---

### 2.2 Model Configuration (`rknn.config`)

```python
rknn.config(
    mean_values=None,
    std_values=None,
    quant_img_RGB2BGR=False,
    quantized_dtype='w8a8',
    quantized_algorithm='normal',
    quantized_method='channel',
    float_dtype='float16',
    optimization_level=3,
    target_platform=None,
    custom_string=None,
    remove_weight=False,
    compress_weight=False,
    single_core_mode=False,
    model_pruning=False,
    op_target=None,
    dynamic_input=None,
    quantize_weight=False,
    remove_reshape=False,
    sparse_infer=False,
    enable_flash_attention=False,
    auto_hybrid_cos_thresh=0.98,
    auto_hybrid_euc_thresh=None
)
```

#### RV1103-Specific Parameters

| Parameter | Type | Default | RV1103 Description |
|-----------|------|---------|-------------------|
| `target_platform` | str | `None` | `'rv1103'` or `'rv1103b'` |
| `quantized_dtype` | str | `'w8a8'` | **RV1103 supports:** `w8a8`, `w16a16i`, `w16a16i_dfp` |
| `quantized_algorithm` | str | `'normal'` | `normal`, `mmse`, `kl_divergence`, `gdq` |
| `quantized_method` | str | `'channel'` | `layer`, `channel`, or `group{SIZE}` (32–256, multiple of 32) |
| `dynamic_input` | list | `None` | `[[shape1], [shape2], ...]` for dynamic input shapes |
| `mean_values` | list | `None` | Input mean per channel. `None` = zero. Multi-input: `[[c1,c2,c3], [c1,c2,c3]]` |
| `std_values` | list | `None` | Input std per channel. `None` = one. Multi-input: `[[c1,c2,c3], [c1,c2,c3]]` |
| `quant_img_RGB2BGR` | bool | `False` | Convert RGB→BGR during quantization calibration only |
| `optimization_level` | int | `3` | `3`=all, `2/1`=some disabled, `0`=none |
| `custom_string` | str | `None` | Custom metadata in the RKNN model |

#### Quantization Data Types (RV1103)

| dtype | Weight | Activation | Description |
|-------|--------|------------|-------------|
| `w8a8` | INT8 (asymmetric) | INT8 (asymmetric) | Standard INT8 quantization |
| `w16a16i` | FP16 | FP16 | 16-bit fixed-point |
| `w16a16i_dfp` | Dynamic FP16 | Dynamic FP16 | Dynamic fixed-point |

#### Quantization Algorithms

| Algorithm | Speed | Accuracy | Recommended Samples |
|-----------|-------|----------|-------------------|
| `normal` | Fast | Good | 20–100 |
| `mmse` | Slow | Best | 20–50 |
| `kl_divergence` | Moderate | Better than normal | 20–100 |
| `gdq` | Slow | Best for w4a16/w8a16 | 200+ (w4a16/w8a16 only) |

**Return:** None

---

### 2.3 Loading Models

#### 2.3.1 ONNX

```python
ret = rknn.load_onnx(
    model='./model.onnx',
    inputs=None,          # Input node names (list of str). None = auto-detect
    input_size_list=None, # Input shapes (list of list). Required if inputs set
    input_initial_val=None, # Constant initial values (list of ndarray or None)
    outputs=None          # Output node names (list of str). None = auto-detect
)
```

#### 2.3.2 PyTorch

```python
ret = rknn.load_pytorch(
    model='./model.pt',
    input_size_list=None  # Input shapes (list of list)
)
```

> Model must be in **torchscript format**. QAT models require PyTorch >= 1.9.0.

#### 2.3.3 TensorFlow

```python
ret = rknn.load_tensorflow(
    tf_pb='./model.pb',
    inputs=None,
    input_size_list=None,
    outputs=None,
    input_is_nchw=False  # Default NHWC layout
)
```

#### 2.3.4 TensorFlow Lite

```python
ret = rknn.load_tflite(
    model='./model.tflite',
    input_is_nchw=False
)
```

#### 2.3.5 Caffe

```python
ret = rknn.load_caffe(
    model='./model.prototxt',
    blobs='./model.caffemodel',
    input_name=None  # Input layer names for multi-input models
)
```

#### 2.3.6 DarkNet

```python
ret = rknn.load_darknet(
    model='./model.cfg',
    weight='./model.weights'
)
```

**Return value for all:** `0` = success, `-1` = failure

---

### 2.4 Build RKNN Model

```python
ret = rknn.build(
    do_quantization=True,
    dataset=None,
    rknn_batch_size=None,
    auto_hybrid=False
)
```

| Parameter | Type | Default | Description |
|-----------|------|---------|-------------|
| `do_quantization` | bool | `True` | Enable INT8 quantization |
| `dataset` | str | `None` | Path to text file with image/npy paths (one per line) |
| `rknn_batch_size` | int | `None` | Batch size for NPU multi-core utilization |
| `auto_hybrid` | bool | `False` | Auto-convert overflow layers to FP16 (requires `auto_hybrid_cos_thresh` config) |

**Return value:** `0` = success, `-1` = failure

> **Dataset format:** Text file, one file path per line. For multi-input models, separate paths with spaces on each line.

---

### 2.5 Export RKNN Model

```python
ret = rknn.export_rknn(export_path='./model.rknn')
```

| Parameter | Type | Description |
|-----------|------|-------------|
| `export_path` | str | Output file path (`.rknn` extension) |

**Return value:** `0` = success, `-1` = failure

---

### 2.6 Load RKNN Model (for Deployment)

```python
ret = rknn.load_rknn(path='./model.rknn')
```

> After loading an RKNN model, you skip config/load/build steps. The model can only be used for board-side inference or performance evaluation — **not** for simulator or accuracy analysis.

| Parameter | Type | Description |
|-----------|------|-------------|
| `path` | str | Path to `.rknn` file |

**Return value:** `0` = success, `-1` = failure
---

## 3. Runtime APIs

### 3.1 Initialize Runtime (`rknn.init_runtime`)

```python
ret = rknn.init_runtime(
    target=None,
    device_id=None,
    perf_debug=False,
    eval_mem=False,
    async_mode=False,
    core_mask=None,
    fallback_prior_device='cpu'
)
```

| Parameter | Type | Default | RV1103 Description |
|-----------|------|---------|-------------------|
| `target` | str | `None` | `'rv1103'`, `'rv1103b'`, or `None` (simulator) |
| `device_id` | str | `None` | Device ID for multi-board setups (from `list_devices()`) |
| `perf_debug` | bool | `False` | Enable per-layer timing data |
| `eval_mem` | bool | `False` | Enable memory evaluation mode |
| `async_mode` | bool | `False` | Asynchronous inference (not yet supported) |
| `core_mask` | int | `RKNN.NPU_CORE_AUTO` | NPU core selection (RV1103 has single core) |
| `fallback_prior_device` | str | `'cpu'` | Fallback for unsupported ops: `'cpu'` or `'gpu'` |

**Return value:** `0` = success, `-1` = failure

---

### 3.2 Inference (`rknn.inference`)

```python
results = rknn.inference(
    inputs=None,
    data_format=None,
    inputs_pass_through=None
)
```

| Parameter | Type | Default | Description |
|-----------|------|---------|-------------|
| `inputs` | list of ndarray | Required | Input data tensors |
| `data_format` | str | `'nhwc'` | Input layout: `'nchw'` or `'nhwc'` (4-D only) |
| `inputs_pass_through` | list of int | `None` | `[1, 0, ...]` — 1 = skip preprocessing, 0 = apply preprocessing |

> **pass_through mode:** Skips mean subtraction and std division that were embedded during `rknn.config()`.

**Return value:** List of output ndarrays (one per output node)

---

### 3.3 Evaluate Performance (`rknn.eval_perf`)

```python
perf_result = rknn.eval_perf(is_print=True, fix_freq=True)
```

| Parameter | Type | Default | Description |
|-----------|------|---------|-------------|
| `is_print` | bool | `True` | Print performance info to console |
| `fix_freq` | bool | `True` | Fix hardware frequency for consistent results |

**Return value:** String containing performance information

> Requires board connection. If `perf_debug=False` at init, only total model time is returned. If `perf_debug=True`, per-layer timing is included.

---

### 3.4 Evaluate Memory (`rknn.eval_memory`)

```python
memory_detail = rknn.eval_memory(is_print=True)
```

| Parameter | Type | Default | Description |
|-----------|------|---------|-------------|
| `is_print` | bool | `True` | Print memory info to console |

**Return value:** Dictionary:
```python
{
    'weight_memory': 3698688,    # Weight footprint (bytes)
    'internal_memory': 1756160,  # Internal tensor memory (bytes)
    'other_memory': 484352,      # Other tensor memory (bytes)
    'total_memory': 5939200      # Total memory (bytes)
}
```

> Requires `eval_mem=True` at `init_runtime()`.

---

### 3.5 Get SDK Version (`rknn.get_sdk_version`)

```python
sdk_version = rknn.get_sdk_version()
```

> Must call after `load_rknn()` and `init_runtime()`.

**Return value:** String containing API, rknn_server, and rknnrt versions.

---

### 3.6 List Devices (`rknn.list_devices`)

```python
adb_devices, ntb_devices = rknn.list_devices()
```

> Lists all connected RV1103 boards. Two connection modes: ADB and NTB.

**Return value:** Tuple of two lists — `(adb_device_ids, ntb_device_ids)`

---

## 4. Advanced APIs

### 4.1 Hybrid Quantization

#### `hybrid_quantization_step1`

```python
ret = rknn.hybrid_quantization_step1(
    dataset=None,
    rknn_batch_size=None,
    proposal=False,
    proposal_dataset_size=1,
    custom_hybrid=None
)
```

| Parameter | Type | Default | Description |
|-----------|------|---------|-------------|
| `dataset` | str | `None` | Calibration dataset path (same format as `build()`) |
| `rknn_batch_size` | int | `None` | Batch size |
| `proposal` | bool | `False` | Generate hybrid quantization suggestions |
| `proposal_dataset_size` | int | `1` | Number of samples for proposal (time-consuming) |
| `custom_hybrid` | list | `None` | `[[input_name, output_name], ...]` to manually select layers |

**Generates:** `<model>.model`, `<model>.data`, `<model>.quantization.cfg`

**Return value:** `0` = success, `-1` = failure

#### `hybrid_quantization_step2`

```python
ret = rknn.hybrid_quantization_step2(
    model_input=None,
    data_input=None,
    model_quantization_cfg=None
)
```

| Parameter | Type | Description |
|-----------|------|-------------|
| `model_input` | str | Path to `<model>.model` from step 1 |
| `data_input` | str | Path to `<model>.data` from step 1 |
| `model_quantization_cfg` | str | Path to modified `<model>.quantization.cfg` |

**Return value:** `0` = success, `-1` = failure

---

### 4.2 Accuracy Analysis (`rknn.accuracy_analysis`)

```python
ret = rknn.accuracy_analysis(
    inputs=None,
    output_dir='./snapshot',
    target=None,
    device_id=None
)
```

| Parameter | Type | Default | Description |
|-----------|------|---------|-------------|
| `inputs` | list of str | Required | Paths to calibration images (jpg/png/bmp/npy) |
| `output_dir` | str | `'./snapshot'` | Output directory for snapshots |
| `target` | str | `None` | Target platform: `'rv1103'`, `'rv1103b'`, or `None` (simulator) |
| `device_id` | str | `None` | Device ID for multi-board setups |

> Must be called after `build()` or `hybrid_quantization_step2()`.

**Output files:**
- `simulator/` — Quantized model per-layer results on simulator
- `golden/` — Floating-point model per-layer results on simulator
- `runtime/` — NPU board results (when `target` is set)
- `error_analysis.txt` — Cosine and Euclidean distance per layer

**Return value:** `0` = success, `-1` = failure

---

### 4.3 Register Custom Operator (`rknn.reg_custom_op`)

```python
ret = rknn.reg_custom_op(custom_op)
```

| Parameter | Type | Description |
|-----------|------|-------------|
| `custom_op` | class | Custom operator class implementing `shape_infer()` and `compute()` |

> Only supported for ONNX models. Operator type should start with `'cst'`.

**Example:**
```python
from rknn.api.custom_op import get_node_attr

class cstSoftmax:
    op_type = 'cstSoftmax'

    def shape_infer(self, node, in_shapes, in_dtypes):
        return in_shapes.copy(), in_dtypes.copy()

    def compute(self, node, inputs):
        x = inputs[0]
        axis = get_node_attr(node, 'axis')
        x_max = np.max(x, axis=axis, keepdims=True)
        tmp = np.exp(x - x_max)
        s = np.sum(tmp, axis=axis, keepdims=True)
        return [tmp / s]

ret = rknn.reg_custom_op(cstSoftmax)
```

**Return value:** `0` = success, `-1` = failure

---

### 4.4 Generate C++ Deployment Example (`rknn.codegen`)

```python
ret = rknn.codegen(
    output_path='./rknn_app_demo',
    inputs=None,
    overwrite=False
)
```

| Parameter | Type | Default | Description |
|-----------|------|---------|-------------|
| `output_path` | str | Required | Output directory for generated C++ code |
| `inputs` | list of str | `None` | Input image/npy paths for sample code |
| `overwrite` | bool | `False` | Overwrite existing files in output_path |

**Return value:** `0` = success, `-1` = failure

---

### 4.5 Export Encrypted RKNN Model (`rknn.export_encrypted_rknn_model`)

> **Not supported on RV1103/RV1103B/RV1106/RV1106B/RK2118.**

```python
ret = rknn.export_encrypted_rknn_model(
    input_model=None,
    output_model=None,
    crypt_level=1
)
```

| Parameter | Type | Default | Description |
|-----------|------|---------|-------------|
| `input_model` | str | Required | Path to RKNN model to encrypt |
| `output_model` | str | `None` | Output path (default: `{name}.crypt.rknn`) |
| `crypt_level` | int | `1` | Encryption level: 1, 2, or 3 (higher = more secure, slower) |

**Return value:** `0` = success, `-1` = failure

---

## 5. Quick Reference — Complete Conversion Flow

```python
from rknn.api import RKNN

# 1. Initialize
rknn = RKNN(verbose=True)

# 2. Configure
rknn.config(
    mean_values=[[123.675, 116.28, 103.53]],
    std_values=[[58.395, 57.12, 57.375]],
    target_platform='rv1103',
    quantized_dtype='w8a8',
    quantized_algorithm='normal',
    quantized_method='channel'
)

# 3. Load model
ret = rknn.load_onnx(model='./model.onnx')
assert ret == 0

# 4. Build
ret = rknn.build(do_quantization=True, dataset='./dataset.txt')
assert ret == 0

# 5. Export
ret = rknn.export_rknn(export_path='./model.rknn')
assert ret == 0

# 6. Release
rknn.release()
```

---

## 6. Quick Reference — Complete Inference Flow

```python
from rknn.api import RKNN

# 1. Initialize
rknn = RKNN()

# 2. Load RKNN model
ret = rknn.load_rknn(path='./model.rknn')
assert ret == 0

# 3. Initialize runtime on board
ret = rknn.init_runtime(target='rv1103')
assert ret == 0

# 4. Inference
outputs = rknn.inference(inputs=[input_data])

# 5. Performance evaluation (optional)
perf = rknn.eval_perf()

# 6. Memory evaluation (optional)
mem = rknn.eval_memory()

# 7. Release
rknn.release()
```
