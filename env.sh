#!/usr/bin/env bash

export TOOLCHAIN_ROOT="$HOME/.luckfox-pico-toolchain/arm-rockchip830-linux-uclibcgnueabihf"

export TARGET_TRIPLE="arm-rockchip830-linux-uclibcgnueabihf"

export SYSROOT="$TOOLCHAIN_ROOT/arm-rockchip830-linux-uclibcgnueabihf/sysroot"

export PATH="$TOOLCHAIN_ROOT/bin:$PATH"

export CC="${TARGET_TRIPLE}-gcc"
export CXX="${TARGET_TRIPLE}-g++"
export AR="${TARGET_TRIPLE}-ar"
export AS="${TARGET_TRIPLE}-as"
export LD="${TARGET_TRIPLE}-ld"
export NM="${TARGET_TRIPLE}-nm"
export STRIP="${TARGET_TRIPLE}-strip"
export OBJCOPY="${TARGET_TRIPLE}-objcopy"
export OBJDUMP="${TARGET_TRIPLE}-objdump"
export GDB="${TARGET_TRIPLE}-gdb"

echo "RV1103 Toolchain Loaded"
echo "CC=$CC"
echo "CXX=$CXX"
echo "SYSROOT=$SYSROOT"