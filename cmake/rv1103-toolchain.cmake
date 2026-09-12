set(CMAKE_SYSTEM_NAME Linux)
set(CMAKE_SYSTEM_PROCESSOR arm)

set(TOOLCHAIN_ROOT $ENV{TOOLCHAIN_ROOT})
set(SYSROOT $ENV{SYSROOT})

set(CMAKE_SYSROOT ${SYSROOT})

set(CMAKE_C_COMPILER
    ${TOOLCHAIN_ROOT}/bin/arm-rockchip830-linux-uclibcgnueabihf-gcc)

set(CMAKE_CXX_COMPILER
    ${TOOLCHAIN_ROOT}/bin/arm-rockchip830-linux-uclibcgnueabihf-g++)

set(CMAKE_C_FLAGS
    "-march=armv7-a -mfpu=neon-vfpv4 -mfloat-abi=hard")

set(CMAKE_CXX_FLAGS
    "-march=armv7-a -mfpu=neon-vfpv4 -mfloat-abi=hard")

set(CMAKE_FIND_ROOT_PATH ${SYSROOT})

set(CMAKE_FIND_ROOT_PATH_MODE_PROGRAM NEVER)
set(CMAKE_FIND_ROOT_PATH_MODE_LIBRARY ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_INCLUDE ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_PACKAGE ONLY)