# PCGKollect

PCGKollect is a project to collect PCG (phonocardiogram) data efficiently. This repo uses the **Luckfox Pico (RV1103)** to collect PCG PCM data streamed from an **ESP32** and performs **AI processing** on the board.

What runs on the board (`RVFirmware`, started at boot by `/etc/init.d/S99pcgkolect`):

- **Link** to the ESP32 on `/dev/ttyS3` (4 Mbaud after negotiation): audio frames plus typed
  control / file-chunk messages; lost audio frames are zero-filled and recorded as gaps.
- **Quality model**: INT8 segmentation U-Net on the NPU + Viterbi beat decoder
  ([PCGKolect_model](https://github.com/cosmosapien200/PCGKolect_model)). A beat is usable when
  its confidence ≥ the dataset threshold (default 80 %); clipping never rejects data.
- **Session engine**: dataset policy from the phone app, auto-start after 2 s of contiguous
  usable beats (pre-trigger audio kept), Force Start, completion targets, crash-safe recordings
  in `/root/recordings/<recording_id>/` (WAV + beats/quality sidecars + metadata), manifest,
  hash-checked ACK before delete.
- **OLED**: quality bar, WIP/DONE, usable/needed time, progress bar.

The phone app and protocols live in [PCGKolect_app](https://github.com/cosmosapien200/PCGKolect_app)
(`docs/PROTOCOL.md`); the ESP32 firmware in [PCGKolect](https://github.com/cosmosapien200/PCGKolect).
Developer guide: [AGENTS.md](AGENTS.md); history: [DEVLOG.md](DEVLOG.md).

## Prerequisites

- Linux host
- Luckfox Pico (RV1103) board with USB RNDIS enabled
- ESP32 streaming PCG PCM data
- Luckfox toolchain installed
- CMake, `make`, `ssh`, `scp`

## Setup

```bash
# 1. Load the cross-compile environment
source env.sh

# 2. Configure the build
make init
```

## Build & Run

```bash
# 1. Build the firmware
make build

# 2. Bring up the USB RNDIS link with the board
make connect

# 3. Upload the binary to the board
make upload

# 4. Run it
make run

# 5. (Optional) Drop into a shell on the board
make shell
```

Or the whole build → upload → run loop in one shot (after `make connect`):

```bash
make dev
```

## Make Targets

| Target      | Description                                          |
|-------------|------------------------------------------------------|
| `make init` | Configure CMake with the RV1103 toolchain            |
| `make build`| Compile and link the firmware                        |
| `make clean`| Remove the `build/` directory                        |
| `make rebuild` | `clean` + `init` + `build` in one go             |
| `make connect` | Configure the USB RNDIS interface and host IP    |
| `make upload` | `scp` the binary to the board                     |
| `make run`  | `ssh` into the board and execute the binary          |
| `make shell`| Drop into an `ssh` shell on the board                |
| `make dev`  | `build` + `upload` + `run`                           |
| `make upload-model` | push the RKNN model from `../PCGKolect_model/release/` |
| `make install-service` | install the boot script (firmware starts at power-up) |
| `make restart` | restart the firmware service after an upload      |
| `make pull-recordings` | copy `/root/recordings` to `./recordings/`   |

## Layout

```
├── CMakeLists.txt            build definition (+ toolchain via cmake/)
├── Makefile                  daily-driver commands (init/build/upload/run/...)
├── env.sh                    loads the cross-compilation environment
├── cmake/rv1103-toolchain.cmake   RV1103 Arm toolchain file
├── include/                  headers
├── src/                      firmware source
├── .vscode/                  clangd / VS Code config
└── build/                    generated at build time (git-ignored)
```