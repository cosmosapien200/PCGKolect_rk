cmake_minimum_required(VERSION 3.20)

project(RVFirmware)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

add_executable(
    ${PROJECT_NAME}

    src/main.cpp
    src/system_info.cpp
)

target_include_directories(
    ${PROJECT_NAME}
    PRIVATE
        include
)

target_link_libraries(
    ${PROJECT_NAME}
    PRIVATE
        pthread
)