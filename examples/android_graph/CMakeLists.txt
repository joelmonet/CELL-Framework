cmake_minimum_required(VERSION 3.24)
project(CellAndroidGraph LANGUAGES CXX)

if(NOT ANDROID)
    message(FATAL_ERROR "Configure the Android graph bridge with the Android NDK toolchain")
endif()
set(CELL_ANDROID_SDK_ROOT "" CACHE PATH "Directory containing one installed Cell SDK per ABI")
if(CELL_ANDROID_SDK_ROOT AND NOT Cell_DIR)
    set(Cell_DIR "${CELL_ANDROID_SDK_ROOT}/${ANDROID_ABI}/lib/cmake/Cell")
endif()
find_package(Cell CONFIG REQUIRED COMPONENTS AudioFiles)

add_library(cell_android_graph SHARED bridge.cpp)
target_link_libraries(cell_android_graph PRIVATE Cell::AudioFiles)
set_target_properties(cell_android_graph PROPERTIES
    CXX_VISIBILITY_PRESET hidden VISIBILITY_INLINES_HIDDEN ON)
