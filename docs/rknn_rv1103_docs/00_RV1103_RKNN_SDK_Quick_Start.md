# RV1103 RKNN SDK Quick Start

> **Version:** V2.3.2 | **Release Date:** 2025-04-03
>
> Focused on RV1103 / RV1103B platforms.

---

## 1. Overview

This guide covers how to use RKNN-Toolkit2 on a computer to convert AI models and deploy them to an RV1103 development board using RKNPU2.

**Supported platforms:** RV1103, RV1103B

### 1.1 Directory Structure

```
Projects/
├── rknn-toolkit2/       # RKNN-Toolkit2 repository
│   ├── doc/
│   ├── rknn-toolkit2/
│   │   ├── packages/
│   │   └── ...
│   └── rknpu2/
│       ├── runtime/
│       └── ...
└── rknn_model_zoo/      # RKNN Model Zoo repository
    ├── datasets/
    ├── examples/
    └── ...
```

---

## 2. Prepare Development Board

### 2.1 Required Tools

| Item | Specification |
|------|--------------|
| Data cable | Micro USB data cable |
| Power adapter | 12V-2A output |

### 2.2 Connect Board to Computer

1. Prepare a computer with **Ubuntu 18.04 / 20.04 / 22.04**.
2. Connect the power adapter to the development board.
3. Connect the board to the computer via the **OTG** USB port (look for "OTG" label on the board).
4. Power on the board and wait for the system to boot.
5. Verify the connection:

```bash
# If adb is not installed:
sudo apt install adb

# Verify connection:
adb devices
# Expected output:
# List of devices attached
# <device_id>  device
```

---

## 3. Prepare Development Environment

### 3.1 Clone Repositories

```bash
mkdir Projects && cd Projects
git clone https://github.com/airockchip/rknn-toolkit2.git --depth 1
git clone https://github.com/airockchip/rknn_model_zoo.git --depth 1
```

### 3.2 Install RKNN-Toolkit2 on Computer

#### 3.2.1 Install Python (via Miniforge)

```bash
# Install Miniforge
wget -c https://github.com/conda-forge/miniforge/releases/latest/download/Miniforge3-Linux-x86_64.sh
chmod 777 Miniforge3-Linux-x86_64.sh
bash Miniforge3-Linux-x86_64.sh

# Create Python 3.8 environment (recommended version)
source ~/miniforge3/bin/activate
conda create -n toolkit2 python=3.8
conda activate toolkit2
# Prompt changes to: (toolkit2) user@host:~$
```

#### 3.2.2 Install RKNN-Toolkit2

**Via pip:**
```bash
pip install rknn-toolkit2 -i https://pypi.org/simple
# Or upgrade:
pip install rknn-toolkit2 -i https://pypi.org/simple --upgrade
```

**Via local wheel package:**
```bash
cd Projects/rknn-toolkit2/rknn-toolkit2
pip install -r packages/x86_64/requirements_cpxx.txt   # cpxx = Python version
pip install packages/x86_64/rknn_toolkit2-x.x.x-cpxx-cpxx-manylinux_2_17_x86_64.manylinux2014_x86_64.whl
```

#### 3.2.3 Verify Installation

```bash
python -c "from rknn.api import RKNN; print('OK')"
```

### 3.3 Install Compilation Tools

#### 3.3.1 Install CMake

```bash
sudo apt update
sudo apt install cmake
```

#### 3.3.2 Install GCC Cross-Compiler

1. Check the board architecture:
   ```bash
   adb shell uname -a
   # RV1103 outputs: armv7l (armhf architecture)
   ```

2. Download and extract GCC from: https://console.zbox.filez.com/l/H1fV9a (Extraction code: `rknn`)

3. Extract to the `Projects` folder. The compiler path will be:
   ```
   Projects/arm-rockchip830-linux-uclibcgnueabihf/bin/arm-rockchip830-linux-uclibcgnueabihf
   ```

### 3.4 Install RKNPU2 Environment on the Board

#### 3.4.1 Confirm RKNPU2 Driver Version

```bash
dmesg | grep -i rknpu
# or
cat /sys/kernel/debug/rknpu/version
# Expected: RKNPU driver: vX.X.X
```

> **Recommendation:** RKNPU2 driver version >= 0.9.2

#### 3.4.2 Check RKNPU2 Environment

The RV1103 RKNPU2 environment consists of:
- **rknn_server**: Background proxy service on the board (receives data from PC via USB, executes on board Runtime, returns results)
- **librknnmrt.so**: Runtime library that loads RKNN models and runs inference on the NPU

Check if rknn_server is running:
```bash
adb shell
restart_rknn.sh
# Expected output: start rknn server, version: x.x.x

# Check versions:
strings /oem/usr/bin/rknn_server | grep -i "rknn_server version"
strings /oem/usr/lib/librknnmrt.so | grep -i "librknnmrt version"
# Both should show the same version (e.g., x.x.x)
```

> **Important version notes for RV1103:**
> - Dynamic input dimensions: RKNN Server & Runtime >= 1.5.0
> - Models > 2 GB: rknn_server >= 2.0.0b0
> - **RV1103 small memory platforms: rknn_server >= 2.1.0 recommended**
> - All three (RKNN Server, Runtime, RKNN-Toolkit2) versions should be consistent

#### 3.4.3 Install/Update RKNPU2 Environment

If versions are inconsistent or rknn_server is missing:

```bash
cd Projects/rknn-toolkit2/rknpu2

# Push rknn_server to board
adb push runtime/Linux/rknn_server/armhf-uclibc/usr/bin/* /oem/usr/bin

# Push librknnmrt.so
adb push runtime/Linux/librknn_api/armhf-uclibc/librknnmrt.so /oem/usr/lib

# Set permissions
adb shell
chmod +x /oem/usr/bin/rknn_server
chmod +x /oem/usr/bin/start_rknn.sh
chmod +x /oem/usr/bin/restart_rknn.sh

# Restart rknn_server
restart_rknn.sh
```

---

## 4. Run Example Programs

### 4.1 RKNN Model Zoo Structure

```
rknn_model_zoo/
├── 3rdparty/          # Third-party libraries
├── datasets/          # Datasets for evaluation
├── examples/          # Example code
│   └── yolov5/
│       ├── cpp/       # C/C++ version
│       ├── model/     # Models, test images
│       └── python/    # Python version
└── ...
```

Each model example provides both Python and C/C++ sample code.

### 4.2 Run RKNN Python Demo

Taking YOLOv5 as an example.

#### 4.2.1 Prepare Model

```bash
cd Projects/rknn_model_zoo/examples/yolov5/model
./download_model.sh
# Downloads ONNX model (e.g., model/yolov5s_relu.onnx)
```

#### 4.2.2 Convert Model to RKNN Format

```bash
cd Projects/rknn_model_zoo/examples/yolov5/python
# Usage: python convert.py model_path [rv1103|rv1103b] [i8/fp] [output_path]
python convert.py ../model/yolov5s_relu.onnx rv1103 i8 ../model/yolov5s_relu.rknn
```

> **Note:** Models compiled for RV1103/RV1103B cannot be shared with RV1106/RV1106B.

#### 4.2.3 Run the Demo

```bash
cd Projects/rknn_model_zoo/examples/yolov5/python
# Usage: python yolov5.py --model_path {rknn_model} --target {target_platform} [--img_show]
python yolov5.py --model_path ../model/yolov5s_relu.rknn --target rv1103 --img_show
```

> `--img_show` displays the result image. Without it, results are printed to console.

#### 4.2.4 Accuracy Evaluation (Optional)

```bash
# Download COCO dataset
cd Projects/rknn_model_zoo/datasets/COCO
pip install pycocotools
python download_eval_dataset.py

# Run accuracy evaluation
cd ../examples/yolov5/python
python yolov5.py \
  --model_path ../model/yolov5s_relu.rknn \
  --target rv1103 \
  --img_folder ../../../datasets/COCO/val2017 \
  --coco_map_test
```

### 4.3 Run RKNN C Demo

#### 4.3.1 Prepare Model

```bash
cd Projects/rknn_model_zoo/examples/yolov5/model
./download_model.sh
```

#### 4.3.2 Convert Model

```bash
cd Projects/rknn_model_zoo/examples/yolov5/python
python convert.py ../model/yolov5s_relu.onnx rv1103 i8 ../model/yolov5s_relu.rknn
```

#### 4.3.3 Compile

Set the GCC compiler path in `build-linux.sh`:

```bash
# Add to the beginning of build-linux.sh:
GCC_COMPILER=Projects/arm-rockchip830-linux-uclibcgnueabihf/bin/arm-rockchip830-linux-uclibcgnueabihf
```

Run the build script:

```bash
cd Projects/rknn_model_zoo
# Usage: ./build-linux.sh -t <target> -a <arch> -d <demo_name> [-b <build_type>]
# -t : rv1103 or rv1103b
# -a : armhf (RV1103 uses armhf architecture)
# -d : demo name (e.g., yolov5, MobileNet)
./build-linux.sh -t rv1103 -a armhf -d yolov5
```

#### 4.3.4 Push Files to Board

```bash
cd Projects/rknn_model_zoo
# Push compiled demo to board
adb push install/rv1103_linux_armhf/rknn_yolov5_demo /data/
```

The `install/` directory structure:
```
install/
└── rv1103_linux_armhf/
    └── rknn_yolov5_demo/
        ├── lib/           # Dependency libraries
        ├── model/         # Models, test images
        └── rknn_yolov5_demo  # Executable
```

#### 4.3.5 Run on Board

```bash
adb shell
cd /data/rknn_yolov5_demo/
export LD_LIBRARY_PATH=/data/rknn_yolov5_demo/lib
# Usage: ./rknn_yolov5_demo <model_path> <input_path>
./rknn_yolov5_demo model/yolov5s_relu.rknn model/bus.jpg
```

Output image is saved at `rknn_yolov5_demo/out.png`. Pull it back:

```bash
adb pull /data/rknn_yolov5_demo/out.png .
```

---

## 5. Troubleshooting

### 5.1 `adb devices` Shows Nothing

1. Reconnect the USB cable, try a different USB port, or replace the cable.
2. Ensure only one adb server is running. If connecting from Docker, kill the host's adb server first:
   ```bash
   adb kill-server
   ```
3. If `command 'adb' not found`, install it:
   ```bash
   sudo apt install adb
   ```

### 5.2 rknn_server Not Running

Some boards don't start rknn_server automatically. Manually start it:

```bash
adb shell
restart_rknn.sh
```

If running RKNN Python Demo fails with:
```
E init_runtime: The rknn_server on the connected device is abnormal
```

Start rknn_server manually (see section 3.4.2).

### 5.3 Insufficient Memory — Model Loading Fails

RV1103 has limited memory. If the `rkipc` process is running, it can cause model loading failures:
```
E init_runtime: Exception: RKNN init failed. error code: RKNN_ERR_MODEL_INVALID
```

Kill the `rkipc` process:

```bash
adb shell
top                    # Find the PID of rkipc
kill -9 <PID>
```

---

## 6. Quick Reference

| Component | RV1103 Path/Version |
|-----------|-------------------|
| Runtime library | `/oem/usr/lib/librknnmrt.so` |
| RKNN Server binary | `/oem/usr/bin/rknn_server` |
| Start script | `restart_rknn.sh` |
| Target platform flag | `rv1103` or `rv1103b` |
| Board architecture | `armhf` (armv7l) |
| GCC cross-compiler | `arm-rockchip830-linux-uclibcgnueabihf` |
| Recommended driver version | >= 0.9.2 |
| Recommended rknn_server version | >= 2.1.0 |
| Python version | 3.8 (recommended) |
| Quantization types | `i8` (INT8), `fp` (FP16) |
