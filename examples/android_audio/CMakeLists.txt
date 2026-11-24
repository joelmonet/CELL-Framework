cmake_minimum_required(VERSION 3.24)
project(CellAndroidAudio LANGUAGES CXX)

if(NOT ANDROID)
    message(FATAL_ERROR "Configure the Android audio probe with the Android NDK toolchain")
endif()
if(NOT TARGET Cell::AudioDevice)
    find_package(Cell CONFIG REQUIRED COMPONENTS AudioDevice)
endif()

add_executable(cell_android_audio_probe main.cpp)
target_link_libraries(cell_android_audio_probe PRIVATE Cell::AudioDevice)
