cmake_minimum_required(VERSION 3.24)
project(CellGainExample VERSION 0.1.0 LANGUAGES CXX)
if(NOT TARGET Cell::Core)
    find_package(Cell CONFIG REQUIRED COMPONENTS Core DSP)
endif()
option(CELL_GAIN_UI "Build the independent reference-free unit editor" OFF)
if(CELL_GAIN_UI AND NOT TARGET Cell::UI)
    find_package(Cell CONFIG REQUIRED COMPONENTS UI)
endif()
add_library(cell_sdk_gain STATIC processor.cpp)
set_target_properties(cell_sdk_gain PROPERTIES POSITION_INDEPENDENT_CODE ON)
target_include_directories(cell_sdk_gain PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}")
target_link_libraries(cell_sdk_gain PUBLIC Cell::DSP)
set(cell_gain_ui_arguments)
if(CELL_GAIN_UI)
    target_sources(cell_sdk_gain PRIVATE editor.cpp)
    target_link_libraries(cell_sdk_gain PUBLIC Cell::UI)
    list(APPEND cell_gain_ui_arguments UI)
endif()

add_executable(cell_sdk_gain_smoke main.cpp)
target_include_directories(cell_sdk_gain_smoke PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/../../tests")
target_link_libraries(cell_sdk_gain_smoke PRIVATE cell_sdk_gain)
target_compile_definitions(cell_sdk_gain_smoke PRIVATE CELL_TEST_REFERENCE_GAIN_STATE)

if(CMAKE_SYSTEM_NAME STREQUAL "Linux" AND TARGET Cell::AudioDevice)
    add_executable(cell_gain_standalone standalone.cpp)
    target_link_libraries(cell_gain_standalone PRIVATE cell_sdk_gain Cell::AudioDevice)
endif()

set(CELL_GAIN_VST2_SDK "" CACHE PATH "Optional separately supplied VST2.4 SDK")
if(CELL_GAIN_VST2_SDK)
    cell_add_vst2_plugin(CellGain_VST2 ${cell_gain_ui_arguments} PROCESSOR cell_sdk_gain
        ID com.example.cell.gain NAME "Cell Gain Example" VST2_SDK "${CELL_GAIN_VST2_SDK}" VST2_ID 0x43474578)
endif()

set(CELL_GAIN_CLAP_SDK "" CACHE PATH "Optional official CLAP checkout")
set(CELL_GAIN_CLAP_EDITOR_DIR "" CACHE PATH "Optional folder with cell-plugin-editor.html and cell-ui-bindings.hpp")
if(CELL_GAIN_CLAP_SDK)
    set(clap_editor_args)
    if(CELL_GAIN_CLAP_EDITOR_DIR)
        if(NOT TARGET Cell::WebView)
            find_package(Cell CONFIG REQUIRED COMPONENTS WebView)
        endif()
        list(APPEND clap_editor_args
            CLAP_EDITOR_HTML "${CELL_GAIN_CLAP_EDITOR_DIR}/cell-plugin-editor.html"
            CLAP_EDITOR_BINDINGS "${CELL_GAIN_CLAP_EDITOR_DIR}/cell-ui-bindings.hpp")
    endif()
    cell_add_clap_plugin(CellGain_CLAP ${cell_gain_ui_arguments} PROCESSOR cell_sdk_gain
        ID com.example.cell.gain NAME "Cell Gain Example" CLAP_SDK "${CELL_GAIN_CLAP_SDK}" ${clap_editor_args})
    if(CELL_GAIN_CLAP_EDITOR_DIR AND CMAKE_SYSTEM_NAME STREQUAL "Darwin")
        enable_language(OBJCXX)
        add_executable(cell_clap_cocoa_check EXCLUDE_FROM_ALL "${CMAKE_CURRENT_SOURCE_DIR}/../../tests/clap_cocoa_editor.mm")
        set_target_properties(cell_clap_cocoa_check PROPERTIES OBJCXX_STANDARD 20
            OBJCXX_STANDARD_REQUIRED ON OBJCXX_EXTENSIONS OFF)
        target_include_directories(cell_clap_cocoa_check PRIVATE "${CELL_GAIN_CLAP_SDK}/include")
        target_link_libraries(cell_clap_cocoa_check PRIVATE "-framework Cocoa" "-framework CoreFoundation" "-framework WebKit")
        add_dependencies(cell_clap_cocoa_check CellGain_CLAP)
    endif()
endif()
set(CELL_GAIN_VST3_SDK "" CACHE PATH "Optional complete official VST3 SDK checkout")
set(CELL_GAIN_VST3_EDITOR_DIR "" CACHE PATH "Optional folder with cell-plugin-editor.html and cell-ui-bindings.hpp")
if(CELL_GAIN_VST3_SDK)
    if(CMAKE_SYSTEM_NAME STREQUAL "Darwin")
        enable_language(OBJCXX)
    endif()
    set(vst3_editor_args)
    if(CELL_GAIN_VST3_EDITOR_DIR)
        if(NOT TARGET Cell::WebView)
            find_package(Cell CONFIG REQUIRED COMPONENTS WebView)
        endif()
        list(APPEND vst3_editor_args
            VST3_EDITOR_HTML "${CELL_GAIN_VST3_EDITOR_DIR}/cell-plugin-editor.html"
            VST3_EDITOR_BINDINGS "${CELL_GAIN_VST3_EDITOR_DIR}/cell-ui-bindings.hpp")
    endif()
    set(SMTG_PLUGIN_TARGET_PATH "" CACHE PATH "Optional explicitly selected plugin installation directory")
    set(SMTG_ADD_VSTGUI OFF CACHE BOOL "" FORCE)
    set(SMTG_ENABLE_VST3_PLUGIN_EXAMPLES OFF CACHE BOOL "" FORCE)
    set(SMTG_ENABLE_VST3_HOSTING_EXAMPLES OFF CACHE BOOL "" FORCE)
    set(SMTG_ENABLE_VSTGUI_SUPPORT OFF CACHE BOOL "" FORCE)
    set(SMTG_CREATE_PLUGIN_LINK OFF CACHE BOOL "" FORCE)
    set(SMTG_RUN_VST_VALIDATOR OFF CACHE BOOL "" FORCE)
    add_subdirectory("${CELL_GAIN_VST3_SDK}" "${CMAKE_CURRENT_BINARY_DIR}/vst3sdk" EXCLUDE_FROM_ALL)
    cell_add_vst3_plugin(CellGain_VST3 ${cell_gain_ui_arguments} PROCESSOR cell_sdk_gain
        ID com.example.cell.gain NAME "Cell Gain Example" COMPANY "Cell SDK Example" CATEGORIES "Fx|Tools" ${vst3_editor_args})
    if(CELL_GAIN_VST3_EDITOR_DIR AND CMAKE_SYSTEM_NAME STREQUAL "Darwin")
        set(vst3_mac_loader "${CELL_GAIN_VST3_SDK}/public.sdk/source/vst/hosting/module_mac.mm")
        add_executable(cell_vst3_cocoa_check EXCLUDE_FROM_ALL
            "${CMAKE_CURRENT_SOURCE_DIR}/../../tests/vst3_cocoa_editor.mm" "${vst3_mac_loader}")
        set_source_files_properties("${vst3_mac_loader}" PROPERTIES COMPILE_OPTIONS "-fobjc-arc")
        set_target_properties(cell_vst3_cocoa_check PROPERTIES OBJCXX_STANDARD 20
            OBJCXX_STANDARD_REQUIRED ON OBJCXX_EXTENSIONS OFF)
        target_link_libraries(cell_vst3_cocoa_check PRIVATE sdk_hosting
            "-framework Cocoa" "-framework CoreFoundation" "-framework WebKit")
        add_dependencies(cell_vst3_cocoa_check CellGain_VST3)
    endif()
endif()

set(CELL_GAIN_AU_SDK "" CACHE PATH "Optional Apple AudioUnitSDK checkout for AUv2")
set(CELL_GAIN_AU_EDITOR_DIR "" CACHE PATH "Optional folder with cell-plugin-editor.html and cell-ui-bindings.hpp")
if(CELL_GAIN_AU_SDK)
    set(au_editor_arguments)
    if(CELL_GAIN_AU_EDITOR_DIR)
        if(NOT TARGET Cell::WebView)
            find_package(Cell CONFIG REQUIRED COMPONENTS WebView)
        endif()
        enable_language(OBJCXX)
        set(au_editor_arguments
            AU_EDITOR_HTML "${CELL_GAIN_AU_EDITOR_DIR}/cell-plugin-editor.html"
            AU_EDITOR_BINDINGS "${CELL_GAIN_AU_EDITOR_DIR}/cell-ui-bindings.hpp" AU_EDITOR_METERS)
    endif()
    cell_add_au_plugin(CellGain_AU PROCESSOR cell_sdk_gain ID com.example.cell.gain.au
        NAME "Cell Gain Example" COMPANY "Cell SDK Example" AU_SDK "${CELL_GAIN_AU_SDK}"
        AU_SUBTYPE CGan AU_MANUFACTURER Cell ${au_editor_arguments})
    if(CELL_SDK_TESTS)
        add_executable(cell_au_gain_host "${CMAKE_CURRENT_SOURCE_DIR}/../../tests/au_gain_host.cpp")
        target_link_libraries(cell_au_gain_host PRIVATE Cell::Core "-framework AudioToolbox" "-framework CoreFoundation")
        if(CELL_GAIN_AU_EDITOR_DIR)
            add_executable(cell_au_cocoa_check EXCLUDE_FROM_ALL "${CMAKE_CURRENT_SOURCE_DIR}/../../tests/au_cocoa_editor.mm")
            set_target_properties(cell_au_cocoa_check PROPERTIES OBJCXX_STANDARD 20
                OBJCXX_STANDARD_REQUIRED ON OBJCXX_EXTENSIONS OFF)
            target_compile_options(cell_au_cocoa_check PRIVATE -fobjc-arc)
            target_link_libraries(cell_au_cocoa_check PRIVATE Cell::Core
                "-framework Cocoa" "-framework AudioToolbox" "-framework WebKit")
            add_dependencies(cell_au_cocoa_check CellGain_AU)
        endif()
    endif()
endif()

set(CELL_GAIN_AAX_SDK "" CACHE PATH "Optional separately supplied Avid AAX SDK")
set(CELL_GAIN_AAX_LIBRARY "" CACHE FILEPATH "Matching prebuilt Avid AAXLibrary archive")
set(CELL_GAIN_AAX_DEBUG_LIBRARY "" CACHE FILEPATH "Optional matching Debug Avid AAXLibrary archive")
if(CELL_GAIN_AAX_SDK OR CELL_GAIN_AAX_LIBRARY OR CELL_GAIN_AAX_DEBUG_LIBRARY)
    if(NOT CMAKE_SYSTEM_NAME STREQUAL "Darwin" AND NOT CMAKE_SYSTEM_NAME STREQUAL "Windows")
        message(FATAL_ERROR "CellGain_AAX requires a Windows or macOS target")
    endif()
    if(NOT CELL_GAIN_AAX_SDK)
        message(FATAL_ERROR "Set CELL_GAIN_AAX_SDK to the Avid SDK root")
    endif()

    set(aax_library_arguments)
    if(CELL_GAIN_AAX_LIBRARY)
        if(NOT EXISTS "${CELL_GAIN_AAX_LIBRARY}" OR IS_DIRECTORY "${CELL_GAIN_AAX_LIBRARY}")
            message(FATAL_ERROR "CELL_GAIN_AAX_LIBRARY must point to a built AAXLibrary")
        endif()
        add_library(cell_gain_aax_library STATIC IMPORTED)
        set_target_properties(cell_gain_aax_library PROPERTIES IMPORTED_LOCATION "${CELL_GAIN_AAX_LIBRARY}")
        list(APPEND aax_library_arguments AAX_LIBRARY cell_gain_aax_library)
    endif()
    if(CELL_GAIN_AAX_DEBUG_LIBRARY)
        if(NOT TARGET cell_gain_aax_library OR NOT EXISTS "${CELL_GAIN_AAX_DEBUG_LIBRARY}"
           OR IS_DIRECTORY "${CELL_GAIN_AAX_DEBUG_LIBRARY}")
            message(FATAL_ERROR "Set both the primary and matching Debug AAXLibrary archives")
        endif()
        set_target_properties(cell_gain_aax_library PROPERTIES
            IMPORTED_CONFIGURATIONS DEBUG
            IMPORTED_LOCATION_DEBUG "${CELL_GAIN_AAX_DEBUG_LIBRARY}"
            MAP_IMPORTED_CONFIG_RELEASE ""
            MAP_IMPORTED_CONFIG_RELWITHDEBINFO ""
            MAP_IMPORTED_CONFIG_MINSIZEREL "")
    endif()
    if(APPLE AND NOT aax_library_arguments)
        enable_language(OBJCXX)
    endif()

    # Reference effect identities only. Products pass their own stable AAX IDs.
    set(aax_gain_arguments PROCESSOR cell_sdk_gain ID com.example.cell.gain
        AAX_SDK "${CELL_GAIN_AAX_SDK}"
        AAX_MANUFACTURER_ID 0x43656C6C AAX_PRODUCT_ID 0x4761696E
        AAX_MONO_ID 0x476E4D6F AAX_STEREO_ID 0x476E5374 AAX_CHUNK_ID 0x476E5376)
    cell_add_aax_plugin(CellGain_AAX ${cell_gain_ui_arguments}
        NAME "Cell Gain Example" ${aax_gain_arguments} ${aax_library_arguments})
    if(CELL_SDK_TESTS)
        if(NOT aax_library_arguments)
            set(aax_library_arguments AAX_LIBRARY CellGain_AAX_sdk)
        endif()
        cell_add_aax_plugin(CellGain_AAX_Test NAME "Cell Gain Qualification"
            ${aax_gain_arguments} ${aax_library_arguments})
        target_sources(CellGain_AAX_Test PRIVATE
            "${CMAKE_CURRENT_SOURCE_DIR}/../../tests/sdk_aax_link_consumer/descriptor.cpp")
    endif()
endif()

set(CELL_GAIN_LV2_SDK "" CACHE PATH "Optional official LV2 SDK checkout or install prefix")
set(CELL_GAIN_LV2_EDITOR_DIR "" CACHE PATH "Optional folder with cell-plugin-editor.html and cell-ui-bindings.hpp")
if(CELL_GAIN_LV2_SDK)
    set(lv2_editor_args)
    if(CELL_GAIN_LV2_EDITOR_DIR)
        if(NOT TARGET Cell::WebView)
            find_package(Cell CONFIG REQUIRED COMPONENTS WebView)
        endif()
        list(APPEND lv2_editor_args
            LV2_EDITOR_HTML "${CELL_GAIN_LV2_EDITOR_DIR}/cell-plugin-editor.html"
            LV2_EDITOR_BINDINGS "${CELL_GAIN_LV2_EDITOR_DIR}/cell-ui-bindings.hpp"
            LV2_EDITOR_METER_PORT 5)
    endif()
    cell_add_lv2_plugin(CellGain_LV2 PROCESSOR cell_sdk_gain
        URI https://example.com/cell/gain
        LV2_SDK "${CELL_GAIN_LV2_SDK}"
        DESCRIPTION "${CMAKE_CURRENT_SOURCE_DIR}/gain.lv2.ttl" ${lv2_editor_args})
endif()

set(CELL_GAIN_LADSPA_INCLUDE_DIR "" CACHE PATH "Directory containing the LADSPA SDK header")
option(CELL_GAIN_DSSI "Also build the DSSI-compatible gain effect" OFF)
if(CELL_GAIN_LADSPA_INCLUDE_DIR)
    cell_add_ladspa_plugin(CellGain_LADSPA PROCESSOR cell_sdk_gain
        INCLUDE_DIR "${CELL_GAIN_LADSPA_INCLUDE_DIR}" LABEL cell_gain UNIQUE_ID 1307321)
endif()
if(CELL_GAIN_DSSI)
    cell_add_ladspa_plugin(CellGain_DSSI DSSI PROCESSOR cell_sdk_gain
        INCLUDE_DIR "${CELL_GAIN_LADSPA_INCLUDE_DIR}" LABEL cell_gain UNIQUE_ID 1307321)
endif()
