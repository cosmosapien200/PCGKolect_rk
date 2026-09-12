PROJECT := RVFirmware
BUILD_DIR := build

TARGET_IP := 172.32.0.81

HOST_IFACE := enp0s20f0u7u4
HOST_IP := 172.32.0.24/24

.PHONY: init build clean rebuild connect upload run shell dev

init:
	cmake -B $(BUILD_DIR) \
		-DCMAKE_TOOLCHAIN_FILE=cmake/rv1103-toolchain.cmake \
		-DCMAKE_EXPORT_COMPILE_COMMANDS=ON

build:
	cmake --build $(BUILD_DIR)

clean:
	rm -rf $(BUILD_DIR)

rebuild: clean init build

connect:
	sudo ip link set $(HOST_IFACE) up
	sudo ip addr flush dev $(HOST_IFACE)
	sudo ip addr add $(HOST_IP) dev $(HOST_IFACE)

upload:
	scp $(BUILD_DIR)/$(PROJECT) root@$(TARGET_IP):/root/

run:
	ssh root@$(TARGET_IP) "/root/$(PROJECT)"

shell:
	ssh root@$(TARGET_IP)

dev: build upload run