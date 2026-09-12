# configure cmake
source env.sh

cmake -B build \
  -DCMAKE_TOOLCHAIN_FILE=cmake/rv1103-toolchain.cmake

# build 
cmake -B build

# connect to rv1103 usb rndis
    # Bring interface up
sudo ip link set enp0s20f0u7u4 up
    # Remove any existing IPs
sudo ip addr flush dev enp0s20f0u7u4
    # Assign static IP
sudo ip addr add 172.32.0.24/24 dev enp0s20f0u7u4

# USE ADB!!
adb shell
adb push <file path on pc> <dest path on rv1103>
adb pull <file path on rv1103>

