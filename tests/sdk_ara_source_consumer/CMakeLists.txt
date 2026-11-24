cmake_minimum_required(VERSION 3.24)
project(CellARASourceConsumer LANGUAGES CXX)
find_package(Cell CONFIG REQUIRED COMPONENTS ARASource)
# Compile the host-oracle contract through installed public headers and exports.
add_executable(cell_sdk_ara_source_probe ../ara_source_reader_tests.cpp)
target_link_libraries(cell_sdk_ara_source_probe PRIVATE Cell::ARASource)
enable_testing()
add_test(NAME installed-ara-source COMMAND cell_sdk_ara_source_probe)
