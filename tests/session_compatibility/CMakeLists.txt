cmake_minimum_required(VERSION 3.24)
project(CellSessionCompatibility LANGUAGES CXX)
find_package(Cell CONFIG REQUIRED COMPONENTS Core DSP AudioFiles Wrappers)

set(CELL_REFERENCE_SOURCE "" CACHE PATH "SDK source for this reference build")
set(CELL_REFERENCE_CLAP_SDK "" CACHE PATH "Pinned CLAP headers")
set(gain_dir "${CELL_REFERENCE_SOURCE}/examples/sdk_gain")
add_library(reference_processor STATIC "${gain_dir}/processor.cpp")
target_link_libraries(reference_processor PUBLIC Cell::DSP)
set_target_properties(reference_processor PROPERTIES POSITION_INDEPENDENT_CODE ON)
cell_add_clap_plugin(reference_gain PROCESSOR reference_processor
    ID com.example.cell.gain NAME CellGainReference CLAP_SDK "${CELL_REFERENCE_CLAP_SDK}")

add_executable(cell_session_reference session.cpp)
target_include_directories(cell_session_reference PRIVATE "${CELL_REFERENCE_CLAP_SDK}/include")
target_link_libraries(cell_session_reference PRIVATE reference_processor Cell::AudioFiles ${CMAKE_DL_LIBS})

if(APPLE)
    set(plugin "$<TARGET_BUNDLE_DIR_NAME:reference_gain>/Contents/MacOS/$<TARGET_FILE_NAME:reference_gain>")
else()
    set(plugin "$<TARGET_FILE_NAME:reference_gain>")
endif()
file(GENERATE OUTPUT "${CMAKE_CURRENT_BINARY_DIR}/$<CONFIG>/paths.txt"
    CONTENT "bin/$<TARGET_FILE_NAME:cell_session_reference>\nplugins/${plugin}\n")
install(FILES "${CMAKE_CURRENT_BINARY_DIR}/$<CONFIG>/paths.txt" DESTINATION .)
install(TARGETS cell_session_reference RUNTIME DESTINATION bin)
install(TARGETS reference_gain LIBRARY DESTINATION plugins BUNDLE DESTINATION plugins)
