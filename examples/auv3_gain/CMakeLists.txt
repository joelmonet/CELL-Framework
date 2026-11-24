add_library(cell_auv3_gain STATIC "${CMAKE_CURRENT_SOURCE_DIR}/../sdk_gain/processor.cpp")
target_link_libraries(cell_auv3_gain PRIVATE Cell::DSP)
set_target_properties(cell_auv3_gain PROPERTIES POSITION_INDEPENDENT_CODE ON)

set(CELL_GAIN_AUV3_EDITOR_DIR "" CACHE PATH "Optional folder with cell-plugin-editor.html and cell-ui-bindings.hpp")
if(CELL_GAIN_AUV3_EDITOR_DIR)
    set(editor_html "${CELL_GAIN_AUV3_EDITOR_DIR}/cell-plugin-editor.html")
    set(editor_bindings "${CELL_GAIN_AUV3_EDITOR_DIR}/cell-ui-bindings.hpp")
else()
    set(editor_html "${CMAKE_CURRENT_SOURCE_DIR}/Cell Gain Editor.html")
    set(editor_bindings "${CMAKE_CURRENT_SOURCE_DIR}/cell-ui-bindings.hpp")
endif()
cell_add_auv3_plugin(CellGain_AUv3 PROCESSOR cell_auv3_gain
    ID com.example.cell.auv3.gain.effect NAME "Cell Gain"
    COMPANY "Cell SDK Example" AU_SUBTYPE Cv3G AU_MANUFACTURER Cell
    AU_EDITOR_HTML "${editor_html}" AU_EDITOR_BINDINGS "${editor_bindings}" AU_EDITOR_METERS)

add_library(cell_auv3_midi_probe STATIC midi_processor.cpp)
target_link_libraries(cell_auv3_midi_probe PRIVATE Cell::Core)
set_target_properties(cell_auv3_midi_probe PROPERTIES POSITION_INDEPENDENT_CODE ON)
cell_add_auv3_plugin(CellMIDIProbe_AUv3 PROCESSOR cell_auv3_midi_probe
    ID com.example.cell.auv3.gain.midiprobe NAME "Cell MIDI Probe"
    COMPANY "Cell SDK Example" AU_TYPE aumi AU_SUBTYPE CtFx AU_MANUFACTURER Cell)

add_library(cell_auv3_instrument_probe STATIC instrument_processor.cpp)
target_link_libraries(cell_auv3_instrument_probe PRIVATE Cell::Core)
set_target_properties(cell_auv3_instrument_probe PROPERTIES POSITION_INDEPENDENT_CODE ON)
cell_add_auv3_plugin(CellInstrumentProbe_AUv3 PROCESSOR cell_auv3_instrument_probe
    ID com.example.cell.auv3.gain.instrumentprobe NAME "Cell Instrument Probe"
    COMPANY "Cell SDK Example" AU_TYPE aumu AU_SUBTYPE CtIn AU_MANUFACTURER Cell)

add_executable(CellAUv3Host MACOSX_BUNDLE main.mm midi_check.mm instrument_check.mm)
set_target_properties(CellAUv3Host PROPERTIES
    MACOSX_BUNDLE_GUI_IDENTIFIER com.example.cell.auv3.gain
    MACOSX_BUNDLE_BUNDLE_NAME "Cell AUv3"
    MACOSX_BUNDLE_BUNDLE_VERSION 1 MACOSX_BUNDLE_SHORT_VERSION_STRING 0.1.0
    XCODE_ATTRIBUTE_PRODUCT_BUNDLE_IDENTIFIER com.example.cell.auv3.gain
    OBJCXX_STANDARD 20 OBJCXX_STANDARD_REQUIRED ON OBJCXX_EXTENSIONS OFF)
target_compile_options(CellAUv3Host PRIVATE -fobjc-arc)
target_link_libraries(CellAUv3Host PRIVATE Cell::Core
    "-framework Foundation" "-framework AudioToolbox" "-framework AVFoundation" "-framework CoreAudioKit")
if(CMAKE_SYSTEM_NAME STREQUAL "iOS")
    set_target_properties(CellAUv3Host PROPERTIES MACOSX_BUNDLE_INFO_PLIST "${CMAKE_CURRENT_SOURCE_DIR}/Info.plist.in")
    target_link_libraries(CellAUv3Host PRIVATE "-framework UIKit")
else()
    set_target_properties(CellAUv3Host PROPERTIES XCODE_ATTRIBUTE_CODE_SIGN_ENTITLEMENTS
        "${CELL_FRAMEWORK_SOURCE_ROOT}/framework/cmake/CellAUv3MacSandbox.entitlements")
endif()
cell_embed_auv3_extensions(CellAUv3Host CellGain_AUv3 CellMIDIProbe_AUv3 CellInstrumentProbe_AUv3)
