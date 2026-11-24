cmake_minimum_required(VERSION 3.24)
project(CellAAXLinkConsumer LANGUAGES CXX)
if(NOT CMAKE_SYSTEM_NAME STREQUAL "Linux")
    message(FATAL_ERROR "This is the Linux link probe. Use CellGain_AAX for a native plug-in.")
endif()
find_package(Cell CONFIG REQUIRED COMPONENTS Core DSP Wrappers)
set(CELL_TEST_AAX_SDK "" CACHE PATH "Supplied Avid AAX SDK root")
if(NOT EXISTS "${CELL_TEST_AAX_SDK}/Interfaces/AAX_Exports.cpp")
    message(FATAL_ERROR "Set CELL_TEST_AAX_SDK to the supplied official AAX SDK")
endif()

# The SDK's Windows autorelease pool implementation is a platform-neutral
# no-op. Use it only for the Linux ELF link probe.
_cell_add_aax_sdk_sources(cell_aax_test_sdk "${CELL_TEST_AAX_SDK}"
    "${CELL_TEST_AAX_SDK}/Libs/AAXLibrary/source/AAX_CAutoreleasePool.Win.cpp")
add_library(cell_aax_test_gain STATIC ../../examples/sdk_gain/processor.cpp)
set_target_properties(cell_aax_test_gain PROPERTIES POSITION_INDEPENDENT_CODE ON)
target_link_libraries(cell_aax_test_gain PRIVATE Cell::DSP)
_cell_plugin_arguments(P PROCESSOR cell_aax_test_gain ID com.example.cell.gain NAME "Cell Gain Example"
    AAX_SDK "${CELL_TEST_AAX_SDK}" AAX_LIBRARY cell_aax_test_sdk
    AAX_MANUFACTURER_ID 0x43656C6C AAX_PRODUCT_ID 0x4761696E
    AAX_MONO_ID 0x476E4D6F AAX_STEREO_ID 0x476E5374 AAX_CHUNK_ID 0x476E5376)
set(P_VERSION_CODE 10000)
_cell_add_aax_adapter(cell_aax_test_module)
target_sources(cell_aax_test_module PRIVATE descriptor.cpp)
target_link_options(cell_aax_test_module PRIVATE "LINKER:--no-undefined")
target_compile_options(cell_aax_test_sdk PRIVATE -Wno-multichar)
target_compile_options(cell_aax_test_module PRIVATE -Wno-multichar)

add_executable(cell_aax_link_host host.cpp)
target_include_directories(cell_aax_link_host PRIVATE "${CELL_TEST_AAX_SDK}/Interfaces/ACF")
target_link_libraries(cell_aax_link_host PRIVATE Cell::Core ${CMAKE_DL_LIBS})
enable_testing()
add_test(NAME cell_aax_link COMMAND cell_aax_link_host "$<TARGET_FILE:cell_aax_test_module>" --exercise)
