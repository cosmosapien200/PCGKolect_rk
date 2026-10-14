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
    src/signal_quality.cpp
    src/seg_model.cpp
    src/seg_quality.cpp
    src/session_recorder.cpp
    src/control_server.cpp
    src/file_service.cpp
    src/sha256.cpp
    lib/cjson/cJSON.c
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
        lib/cjson
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

# --- RKNN runtime (RV1103 NPU): header v2.3.2 from airockchip/rknn-toolkit2, lib pulled from the board ---
set(RKNN_PATH "${CMAKE_SOURCE_DIR}/lib/rknn")
target_include_directories(${PROJECT_NAME} PRIVATE ${RKNN_PATH}/include)

target_link_libraries(
    ${PROJECT_NAME}
    PRIVATE
        pthread
        ${RKNN_PATH}/armhf-uclibc/librknnmrt.so
)

# NPU self-test (board-side numerics check of the exported model)
add_executable(npu_selftest tools/npu_selftest.cpp src/seg_model.cpp)
target_include_directories(npu_selftest PRIVATE include ${RKNN_PATH}/include)
target_link_libraries(npu_selftest PRIVATE ${RKNN_PATH}/armhf-uclibc/librknnmrt.so)

# Board-side replay of recordings through the full v2 pipeline
add_executable(pcg_replay tools/pcg_replay.cpp src/seg_model.cpp src/seg_quality.cpp src/signal_quality.cpp)
target_include_directories(pcg_replay PRIVATE include ${RKNN_PATH}/include)
target_link_libraries(pcg_replay PRIVATE ${RKNN_PATH}/armhf-uclibc/librknnmrt.so)
