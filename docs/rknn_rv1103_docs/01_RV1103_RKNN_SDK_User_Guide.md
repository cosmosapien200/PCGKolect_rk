# RV1103 RKNN SDK User Guide

> **Version:** V2.3.2 | **Release Date:** 2025-04-03
>
> Focused on RV1103 / RV1103B platforms.

---

## 1. Introduction to RKNN

### 1.1 RKNN Software Stack

The RKNN software stack enables deployment of AI models to Rockchip chips. The workflow:

1. Run **RKNN-Toolkit2** on your computer to convert a trained model into `.rknn` format.
2. Deploy the `.rknn` model to the RV1103 board using the **RKNN C API** or **Python API**.

### 1.2 RKNN-Toolkit2 Features

RKNN-Toolkit2 is a PC-side Python development suite that provides:

| Feature | Description |
|---------|-------------|
| Model Conversion | Convert PyTorch, ONNX, TensorFlow, TFLite, Caffe, DarkNet models to RKNN format |
| Quantization | Quantize floating-point models to INT8/fixed-point |
| Model Inference | Run RKNN models on NPU hardware or simulate on PC |
| Performance Evaluation | Measure inference speed on the target board |
| Memory Evaluation | Measure memory usage on the target board |
| Accuracy Analysis | Compare quantized vs. floating-point model outputs layer by layer |
| Model Encryption | Encrypt RKNN models (not supported on RV1103) |

### 1.3 RKNN Runtime Features

RKNN Runtime loads the RKNN model and runs inference on the NPU. Two processing flows are available:

**General API** — Simple, straightforward:
- Data normalization, quantization, layout conversion performed on CPU
- Model inference executed on NPU
- Suitable for most use cases

**Zero-Copy API** — Higher efficiency for virtual address scenarios:
- Data normalization, quantization, and inference on NPU
- Layout conversion and dequantization on CPU or NPU
- Reduces CPU/DDR bandwidth by avoiding data copies
- Supports direct input from camera or decoded data via file descriptors

> **Choose General API** when input data only has virtual addresses.
> **Choose Zero-Copy API** when input data has physical addresses or file descriptors (fd).

### 1.4 RKNN Development Process

The complete workflow has three stages:

1. **Model Conversion** — Convert original model (ONNX, PyTorch, etc.) to `.rknn` format on PC
2. **Model Evaluation** — Verify accuracy, performance, and memory on the board
3. **Board-side Deployment** — Load, run inference, and manage resources on the device

### 1.5 Glossary

| Term | Description |
|------|-------------|
| RKNN Model | Binary file for RKNPU, extension `.rknn` |
| Inference with board | Running the model on the board's NPU via RKNN-Toolkit2 PC-to-board debugging |
| i8 model | INT8 quantized RKNN model (8-bit signed integer) |
| fp16 model | FP16 non-quantized RKNN model (16-bit half-float) |
| NATIVE_LAYOUT | NPU-native memory layout for best performance |
| tensor | Multi-dimensional array of data |
| fd | File descriptor for representing a buffer |

---

## 2. Development Environment Setup

### 2.1 RKNN-Toolkit2 Installation

RKNN-Toolkit2 is available for **x86** and **ARM64**. This guide uses x86.

> **ARM64 note:** ARM64 does not support Docker or adb. It only supports ONNX and PyTorch frameworks. Debugging functions work only on the local board and require `librknnrt.so` at `/usr/lib64`.

#### 2.1.1 Install via Docker

**Install Docker:**
```bash
sudo groupadd docker
sudo usermod -aG docker $USER
newgrp docker
docker run hello-world   # Verify installation
```

**Prepare Image (two options):**

*Option A — Build from Dockerfile:*
```bash
cd Projects/rknn-toolkit2/rknn-toolkit2/docker/docker_file/ubuntu_xx_xx_cpxx
docker build -f Dockerfile_ubuntu_xx_xx_for_cpxx -t rknn-toolkit2:x.x.x-cpxx .
```

*Option B — Load pre-packaged image:*
Download from: https://console.zbox.filez.com/l/I00fc3 (Extraction code: `rknn`)
```bash
docker load --input rknn-toolkit2-x.x.x-cpxx-docker.tar.gz
```

**Run the container:**
```bash
docker run -t -i --privileged \
  -v /dev/bus/usb:/dev/bus/usb \
  -v /Projects/rknn_model_zoo:/rknn_model_zoo \
  rknn-toolkit2:x.x.x-cpxx \
  /bin/bash
```

#### 2.1.2 Install via Pip

**Install Python:**
```bash
sudo apt-get update
sudo apt-get install python3 python3-dev python3-pip
sudo apt-get install libxslt1-dev zlib1z-dev libglib2.0-0 libsm6 libgl1-mesa-glx libprotobuf-dev gcc
```

**Install Conda (Miniforge):**
```bash
wget -c https://github.com/conda-forge/miniforge/releases/latest/download/Miniforge3-Linux-x86_64.sh
chmod 777 Miniforge3-Linux-x86_64.sh
bash Miniforge3-Linux-x86_64.sh

source ~/miniforge3/bin/activate
conda create -n RKNN-Toolkit2 python=3.8
conda activate RKNN-Toolkit2
```

**Install RKNN-Toolkit2:**
```bash
pip install rknn-toolkit2 -i https://pypi.org/simple
```

### 2.2 NPU Device Environment Setup

#### 2.2.1 Confirm NPU Driver Version

```bash
dmesg | grep -i rknpu
# or
cat /sys/kernel/debug/rknpu/version
cat /sys/kernel/debug/rknpu/driver_version
cat /proc/debug/rknpu/driver_version
# Expected: RKNPU driver: vX.X.X
```

> Recommended driver version >= 0.9.2. Rockchip firmware includes the driver by default.

#### 2.2.2 RKNN Server

**RV1103 uses `rknn_server`** as a background proxy service:
- Receives commands and data from PC via USB
- Executes on board Runtime
- Returns results to PC

Check if rknn_server is running:
```bash
adb shell
ps | grep rknn_server
```

Start rknn_server manually:
```bash
nohup /oem/usr/bin/rknn_server >/dev/null &
```

> Rockchip firmware starts rknn_server automatically during boot. If not, start it manually.

#### 2.2.3 RKNN Server & Runtime Library

**RV1103 uses `librknnmrt.so`** (not `librknnrt.so` used on RK35xx series).

| Component | RV1103 Path |
|-----------|------------|
| rknn_server | `/oem/usr/bin/rknn_server` |
| Runtime library | `/oem/usr/lib/librknnmrt.so` |

**Version requirements:**
- Dynamic input dimensions: rknn_server & Runtime >= 1.5.0
- Models > 2 GB: rknn_server >= 2.0.0b0
- **RV1103 (small memory): rknn_server >= 2.1.0 recommended**
- All three versions (rknn_server, Runtime, RKNN-Toolkit2) should match

**Check versions:**
```bash
strings /oem/usr/bin/rknn_server | grep -i "rknn_server version"
strings /oem/usr/lib/librknnmrt.so | grep -i "librknnmrt version"
```

**Update if needed:**
```bash
cd Projects/rknn-toolkit2/rknpu2
adb push runtime/Linux/rknn_server/armhf-uclibc/usr/bin/* /oem/usr/bin
adb push runtime/Linux/librknn_api/armhf-uclibc/librknnmrt.so /oem/usr/lib
adb shell
chmod +x /oem/usr/bin/rknn_server
chmod +x /oem/usr/bin/start_rknn.sh
chmod +x /oem/usr/bin/restart_rknn.sh
restart_rknn.sh
```

#### 2.2.4 View RKNN Server Logs

**Linux system:**
```bash
adb shell
export RKNN_SERVER_LOGLEVEL=5
nohup /oem/usr/bin/rknn_server >/dev/null &
```

---

## 3. RKNN Basic — Model Conversion

### 3.1 Supported Frameworks

| Framework | RV1103 Support |
|-----------|---------------|
| ONNX | Yes (1.7.0 – 1.10.0+) |
| PyTorch | Yes (1.6.0 – 1.13.1, torchscript format) |
| TensorFlow | Yes (1.12.0 – 2.8.0) |
| TensorFlow Lite | Yes (Schema version = 3) |
| Caffe | Yes (1.0) |
| DarkNet | Yes (Commit: 810d7f7) |

### 3.2 Model Conversion Workflow

```
1. Initialize RKNN object
2. Configure model (rknn.config)
3. Load original model (rknn.load_xxx)
4. Build RKNN model (rknn.build)
5. Export RKNN model (rknn.export_rknn)
6. Release resources (rknn.release)
```

### 3.3 Initialize and Release RKNN Object

```python
from rknn.api import RKNN

# Initialize with verbose logging
rknn = RKNN(verbose=True, verbose_file='./build.log')

# ... perform conversion ...

# Release resources
rknn.release()
```

- `verbose=True` prints detailed logs to screen
- `verbose_file` writes logs to a file (requires `verbose=True`)
---

## 3.4 Configure Model (`rknn.config`)

Before building the model, set configuration parameters:

```python
rknn.config(
    mean_values=[[123.675, 116.28, 103.53]],   # Input mean per channel
    std_values=[[58.395, 57.12, 57.375]],        # Input std per channel
    quant_img_RGB2BGR=False,                     # Convert RGB→BGR during quantization calibration
    target_platform='rv1103',                    # Target: 'rv1103' or 'rv1103b'
    quantized_dtype='asymmetric_quantized-8',    # Quantization type
    quantized_algorithm='normal',                # Algorithm: normal, mmse, kl_divergence, gdq
    quantized_method='channel',                  # Method: layer, channel, or group{SIZE}
    optimization_level=3,                        # 3=all, 0=none
    dynamic_input=None,                          # [[shape1], [shape2], ...] for dynamic shapes
    custom_string=None,                          # Custom metadata string
)
```

### Key Parameters for RV1103

| Parameter | Description | RV1103 Notes |
|-----------|-------------|--------------|
| `target_platform` | Target chip | `'rv1103'` or `'rv1103b'` |
| `quantized_dtype` | Quantization type | Use `'asymmetric_quantized-8'` for INT8, or `'w16a16i'` / `'w16a16i_dfp'` for FP16 |
| `quantized_algorithm` | Calibration algorithm | `normal` (fast, 20-100 samples), `mmse` (slower, more accurate, 20-50 samples), `kl_divergence` (moderate, 20-100 samples) |
| `quantized_method` | Quantization scope | `channel` (default, per-channel weights) or `layer` (per-layer) |
| `mean_values` / `std_values` | Normalization | Embedded into RKNN model; no need for CPU-side preprocessing |
| `optimization_level` | Optimization depth | 3 (all enabled) down to 0 (all disabled) |

### Quantization Data Types

| dtype | Weight | Activation | RV1103 Support |
|-------|--------|------------|----------------|
| `w8a8` | INT8 | INT8 | Yes |
| `w16a16i` | FP16 | FP16 | Yes |
| `w16a16i_dfp` | Dynamic FP16 | Dynamic FP16 | Yes |

### Quantization Algorithms

| Algorithm | Speed | Accuracy | Recommended Samples |
|-----------|-------|----------|-------------------|
| `normal` | Fast | Good | 20–100 |
| `mmse` | Slow | Best | 20–50 |
| `kl_divergence` | Moderate | Better than normal | 20–100 |

---

## 3.5 Load Original Model

Use the appropriate loading function based on your framework:

### ONNX (Recommended)

```python
ret = rknn.load_onnx(
    model='./model.onnx',
    inputs=['input_0'],           # Optional: input node names
    input_size_list=[[1, 3, 224, 224]],  # Input tensor shapes
    outputs=['output_0']          # Optional: output node names
)
```

### PyTorch (torchscript format)

```python
ret = rknn.load_pytorch(
    model='./model.pt',
    input_size_list=[[1, 3, 224, 224]]
)
```

### TensorFlow

```python
ret = rknn.load_tensorflow(
    tf_pb='./model.pb',
    inputs=['input_node'],
    outputs=['output_node1', 'output_node2'],
    input_size_list=[[1, 224, 224, 3]]
)
```

### TensorFlow Lite

```python
ret = rknn.load_tflite(model='./model.tflite')
```

### Caffe

```python
ret = rknn.load_caffe(
    model='./model.prototxt',
    blobs='./model.caffemodel'
)
```

### DarkNet

```python
ret = rknn.load_darknet(
    model='./model.cfg',
    weight='./model.weights'
)
```

> **Return value:** `0` = success, `-1` = failure.

---

## 3.6 Build RKNN Model

```python
ret = rknn.build(
    do_quantization=True,
    dataset='./dataset.txt'   # Path to calibration dataset
)
```

### Quantization Dataset Format

Text file with one path per line:

```
./imgs/image001.jpg
./imgs/image002.jpg
./imgs/image003.npy
```

For multi-input models, separate paths with spaces:
```
./img001.jpg ./img001_alt.jpg
./img002.jpg ./img002_alt.jpg
```

> **Recommendation:** Use images that match your deployment scenario. 20–100 samples is typically sufficient.

---

## 3.7 Export RKNN Model

```python
ret = rknn.export_rknn(export_path='./model.rknn')
```

The exported `.rknn` file is deployed to the RV1103 board for inference.

---

## 3.8 Model Conversion via CLI (`rknn_convert`)

Alternatively, use the YAML-based CLI tool:

```bash
python -m rknn.api.rknn_convert -t rv1103 -i ./model_config.yml -o ./output_path
```

### CLI Parameters

| Flag | Description |
|------|-------------|
| `-t` | Target platform: `rv1103`, `rv1103b` |
| `-i` | YAML config file path |
| `-o` | Output directory |
| `-e` | Enable performance evaluation (requires board connection) |
| `-a` | Enable accuracy analysis: `-a "img1.jpg img2.jpg"` |
| `-v` | Verbose logging |
| `-d` | Device ID for multi-device setups: `-d <device_id>` |

### YAML Config Example

```yaml
models:
  name: my_model
  platform: onnx
  model_file_path: ./model.onnx
  quantize: true
  dataset: ./dataset.txt
  subgraphs:
    input_size_list: [1, 3, 224, 224]
    inputs: [input_0]
    outputs: [output_0]
  configs:
    quantized_dtype: asymmetric_quantized-8
    mean_values: [123.675, 116.28, 103.53]
    std_values: [58.395, 57.12, 57.375]
    quantized_algorithm: normal
    quantized_method: channel
    optimization_level: 3
```
---

## 4. Model Evaluation

### 4.1 Load and Run on Board

```python
# Load the pre-converted RKNN model
rknn = RKNN()
rknn.load_rknn(path='./model.rknn')

# Initialize runtime on the RV1103 board
ret = rknn.init_runtime(
    target='rv1103',      # or 'rv1103b'
    device_id=None,        # Omit if only one board connected
    perf_debug=False,      # True for per-layer timing
    eval_mem=False         # True for memory evaluation
)
```

### 4.2 Inference

```python
# Run inference
outputs = rknn.inference(inputs=[input_data])

# For classification models:
# outputs is a list of ndarrays, one per output node
```

### 4.3 Performance Evaluation

```python
# Initialize with perf_debug=True for per-layer timing
rknn.init_runtime(target='rv1103', perf_debug=True)

# Evaluate
perf_detail = rknn.eval_perf(is_print=True, fix_freq=True)
print(perf_detail)
```

> `fix_freq=True` (default) fixes hardware frequency for consistent measurements.

### 4.4 Memory Evaluation

```python
# Initialize with eval_mem=True
rknn.init_runtime(target='rv1103', eval_mem=True)

# Evaluate memory
memory_detail = rknn.eval_memory(is_print=True)
print(memory_detail)
# Returns: {'weight_memory': ..., 'internal_memory': ..., 'other_memory': ..., 'total_memory': ...}
```

Memory breakdown:
- **weight_memory**: Model weight footprint
- **internal_memory**: Internal tensor memory during inference
- **other_memory**: Other tensor memory
- **total_memory**: Sum of all above

### 4.5 Accuracy Analysis

Compare quantized model output against floating-point baseline:

```python
ret = rknn.accuracy_analysis(
    inputs=['./calibration_img1.jpg', './calibration_img2.jpg'],
    output_dir='./snapshot',
    target='rv1103'
)
```

**Output in `output_dir/`:**
- `simulator/` — Per-layer results from quantized model on simulator
- `golden/` — Per-layer results from floating-point model on simulator
- `runtime/` — Per-layer results from NPU on board (when `target` is set)
- `error_analysis.txt` — Cosine distance and Euclidean distance per layer

> **entire_error:** Each layer's input comes from the quantized model chain.
> **single_error:** Each layer's input comes from the floating-point model.

### 4.6 Get SDK Version

```python
# Must call after load_rknn() and init_runtime()
sdk_version = rknn.get_sdk_version()
print(sdk_version)
# Output:
# RKNN VERSION:
# API: x.x.x
# DRV: rknn_server: x.x.x
# DRV: rknnrt: x.x.x
```

---

## 5. Board-side Deployment (C API)

### 5.1 Compilation

From the RV1103 board's perspective, the deployment flow is:

```
1. Load RKNN model
2. Initialize runtime
3. Set input data
4. Run inference
5. Get output data
6. Release resources
```

**Compile with build-linux.sh:**
```bash
cd rknn_model_zoo
./build-linux.sh -t rv1103 -a armhf -d <demo_name>
```

**Compiled output structure:**
```
install/
└── rv1103_linux_armhf/
    └── <demo_name>_demo/
        ├── lib/              # Shared libraries
        ├── model/            # .rknn model + test images
        └── <demo_name>_demo  # Executable
```

### 5.2 Deploy to Board

```bash
# Push to board
adb push install/rv1103_linux_armhf/<demo_name>_demo /data/

# Run on board
adb shell
cd /data/<demo_name>_demo/
export LD_LIBRARY_PATH=/data/<demo_name>_demo/lib
./<demo_name>_demo model/model.rknn model/input.jpg
```

### 5.3 C API Runtime Functions

| Function | Purpose |
|----------|---------|
| `rknn_init()` | Load RKNN model and initialize runtime |
| `rknn_run()` | Execute model inference |
| `rknn_get_io_memory()` | Get input/output tensor memory pointers |
| `rknn_destroy()` | Release all resources |

### 5.4 General API vs Zero-Copy API

| Aspect | General API | Zero-Copy API |
|--------|-------------|---------------|
| Preprocessing | CPU | NPU |
| Inference | NPU | NPU |
| Post-processing | CPU | CPU or NPU |
| Data copying | Yes | No |
| Best for | Standard use cases | Camera input, decoded frames (fd-based) |

---

## 6. Python Inference on Board (RKNN-Toolkit Lite2)

RKNN-Toolkit Lite2 enables Python inference directly on the board without a PC connection.

### 6.1 Requirements

- Board must have RKNPU2 runtime installed (`librknnmrt.so`)
- RKNN-Toolkit Lite2 Python package installed on the board

### 6.2 Basic Flow

```python
from rknn.lite.rknn_lite import RKNNLite

# 1. Initialize
rknn = RKNNLite()

# 2. Load RKNN model
rknn.load_rknn(path='./model.rknn')

# 3. Initialize runtime on board
rknn.init_runtime()
# Or for specific NPU core:
# rknn.init_runtime(runtime_device='rknpu0')

# 4. Prepare input data (numpy array)
input_data = ...

# 5. Run inference
outputs = rknn.inference(inputs=[input_data])

# 6. Post-process outputs
# ...

# 7. Release
rknn.release()
```

### 6.3 Key APIs

| API | Description |
|-----|-------------|
| `load_rknn(path)` | Load RKNN model file |
| `init_runtime(runtime_device)` | Initialize runtime; `rknpu0` = first NPU core |
| `inference(inputs=[...])` | Run inference, returns list of output ndarrays |
| `get_outputs()` | Get inference outputs (after `inference()`) |
| `release()` | Release all resources |

### 6.4 Getting RKNN-Toolkit Lite2

```bash
# On the board
pip install rknn-lite
# or
pip install rknn_lite
```

---

## 7. Advanced Features

### 7.1 Dynamic Shape

Support variable input dimensions at runtime.

**During model conversion:**
```python
rknn.config(
    target_platform='rv1103',
    dynamic_input=[[[1, 3, 224, 224]], [[1, 3, 192, 192]], [[1, 3, 160, 160]]]
)
rknn.build(do_quantization=True, dataset='./dataset.txt')
```

> The original model must support dynamic input (e.g., shape `[1, 3, -1, -1]`).

**During inference:**
```python
rknn.init_runtime(target='rv1103')
# Input shape must match one of the shapes specified in dynamic_input
outputs = rknn.inference(inputs=[data_224x224])
```

> **RV1103 requirement:** RKNN Server and Runtime >= 1.5.0 for dynamic shapes.

### 7.2 Hybrid Quantization

When INT8 quantization causes accuracy loss, hybrid quantization converts overflow/low-accuracy layers to FP16.

**Two-step process:**
```python
# Step 1: Analyze and generate config
ret = rknn.hybrid_quantization_step1(
    dataset='./dataset.txt',
    proposal=False  # Set True to get auto-suggestions
)

# Step 2: Generate hybrid quantized model
ret = rknn.hybrid_quantization_step2(
    model_input='./model.model',
    data_input='./model.data',
    model_quantization_cfg='./model.quantization.cfg'
)
```

**Automatic hybrid quantization** (simpler):
```python
rknn.config(
    target_platform='rv1103',
    auto_hybrid_cos_thresh=0.98,    # Cosine distance threshold
    auto_hybrid_euc_thresh=None      # Euclidean distance threshold (optional)
)
rknn.build(do_quantization=True, dataset='./dataset.txt', auto_hybrid=True)
```

### 7.3 Model Encryption

> **Not supported on RV1103/RV1103B/RV1106/RV1106B/RK2118.**
> Available on RK35xx series and RV1126B only.

### 7.4 Custom Operators

For ONNX models with custom ops not supported by RKNN compiler:

```python
from rknn.api.custom_op import get_node_attr

class cstCustomOp:
    op_type = 'cstCustomOp'

    def shape_infer(self, node, in_shapes, in_dtypes):
        out_shapes = in_shapes.copy()
        out_dtypes = in_dtypes.copy()
        return out_shapes, out_dtypes

    def compute(self, node, inputs):
        # Custom computation logic
        return [result]

rknn.reg_custom_op(cstCustomOp)
```

> Custom operators require additional C-side registration on the board (see User Guide Ch. 5.5 in the original docs).

### 7.5 Multi-Batch Inference

Increase NPU utilization by processing multiple frames per inference:

```python
ret = rknn.build(
    do_quantization=True,
    dataset='./dataset.txt',
    rknn_batch_size=4  # Process 4 frames at once
)
```

> Match `rknn_batch_size` to the number of NPU cores for best results. Input/output shapes will be multiplied accordingly.

---

## 8. Troubleshooting

### 8.1 Model Conversion Fails

| Issue | Solution |
|-------|----------|
| Unsupported operator | Check RKNN Compiler Support Operator List; use `op_target` config to offload to CPU |
| Quantization accuracy loss | Try different algorithms (`mmse`, `kl_divergence`), use hybrid quantization, or increase calibration samples |
| Memory error during build | Reduce `optimization_level` or reduce model size |

### 8.2 Runtime Fails on Board

| Issue | Solution |
|-------|----------|
| `rknn_server` not running | Manually start: `restart_rknn.sh` |
| Version mismatch | Update rknn_server and librknnmrt.so to match RKNN-Toolkit2 version |
| Model too large for memory | Use INT8 quantization, reduce model size, or kill `rkipc` process |
| `RKNN_ERR_MODEL_INVALID` | Check model was compiled for correct target platform |

### 8.3 Accuracy Issues

1. Verify calibration dataset matches deployment scenario
2. Try different quantization algorithms
3. Use accuracy analysis to identify problematic layers
4. Apply hybrid quantization for overflow layers
5. Check if mean/std values are correctly set
6. Compare PC-side simulator output vs. board output

### 8.4 Performance Issues

1. Profile with `perf_debug=True` to identify bottlenecks
2. Use INT8 quantization instead of FP16 for speed
3. Increase `rknn_batch_size` to utilize NPU cores
4. Minimize CPU↔NPU data transfers
5. Use zero-copy API for camera/decoded frame inputs
