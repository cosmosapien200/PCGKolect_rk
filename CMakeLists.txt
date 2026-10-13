cmake_minimum_required(VERSION 3.20)

project(RVFirmware)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

add_executable(
    ${PROJECT_NAME}

    src/main.cpp
    src/system_info.cpp
    src/pcg_stream.cpp
    src/pcg_preprocessor.cpp
    src/display.cpp
)

# --- u8g2 library sources ---
set(U8G2PATH "${CMAKE_SOURCE_DIR}/lib/u8g2")

file(GLOB U8G2_SRC
    ${U8G2PATH}/cppsrc/*.cpp
    ${U8G2PATH}/cppsrc/*.h
    ${U8G2PATH}/csrc/*.c
    ${U8G2PATH}/csrc/*.h
    ${U8G2PATH}/drivers/*.c
    ${U8G2PATH}/drivers/*.h
    ${U8G2PATH}/port/*.c
    ${U8G2PATH}/port/*.h
)

target_sources(${PROJECT_NAME} PRIVATE ${U8G2_SRC})

target_include_directories(
    ${PROJECT_NAME}
    PRIVATE
        include
        ${U8G2PATH}/cppsrc
        ${U8G2PATH}/csrc
        ${U8G2PATH}/drivers
        ${U8G2PATH}/port
)

# u8g2-linux requires these defines for Linux I2C + cdev GPIO
set(CMAKE_C_FLAGS   "${CMAKE_C_FLAGS} -D__ARM_LINUX__ -DPERIPHERY_GPIO_CDEV_SUPPORT=1 -ffunction-sections -fdata-sections")
set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -D__ARM_LINUX__ -DPERIPHERY_GPIO_CDEV_SUPPORT=1 -ffunction-sections -fdata-sections")

# Garbage collect unused functions/sections to shrink binary
set(CMAKE_EXE_LINKER_FLAGS "${CMAKE_EXE_LINKER_FLAGS} -Wl,--gc-sections")

target_link_libraries(
    ${PROJECT_NAME}
    PRIVATE
        pthread
)
