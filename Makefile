PROJECT := RVFirmware
BUILD_DIR := build

TARGET_IP := 172.32.0.81

HOST_IFACE := enp0s20f0u7u3
HOST_IP := 172.32.0.24/24

ADB := adb
REMOTE_DIR := /root
REMOTE_BIN := $(REMOTE_DIR)/$(PROJECT)

.PHONY: init build clean rebuild connect adb-connect upload run shell dev

init:
	cmake -B $(BUILD_DIR) \
		-DCMAKE_TOOLCHAIN_FILE=cmake/rv1103-toolchain.cmake \
		-DCMAKE_EXPORT_COMPILE_COMMANDS=ON

build:
	cmake --build $(BUILD_DIR)

clean:
	rm -rf $(BUILD_DIR)

rebuild: clean init build

# Configure the host USB network interface
connect:
	sudo ip link set $(HOST_IFACE) up
	sudo ip addr flush dev $(HOST_IFACE)
	sudo ip addr add $(HOST_IP) dev $(HOST_IFACE)

# Connect to the target over ADB TCP/IP, if supported
adb-connect:
	$(ADB) connect $(TARGET_IP):5555

# Upload the firmware executable
upload:
	$(ADB) push $(BUILD_DIR)/$(PROJECT) $(REMOTE_BIN)
	$(ADB) shell chmod +x $(REMOTE_BIN)

# Execute the firmware on the target
run:
	$(ADB) shell "$(REMOTE_BIN)"

# Open an interactive shell on the target
shell:
	$(ADB) shell

# Build, upload, and execute
dev: build upload run
