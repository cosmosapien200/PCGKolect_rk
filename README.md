# PCGKollect

PCGKollect is a project to collect PCG (phonocardiogram) data efficiently. This repo uses the **Luckfox Pico (RV1103)** to collect PCG PCM data streamed from an **ESP32** and performs **AI processing** on the board.

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