include_guard(GLOBAL)

# The SDK and product lab share the same native services and embedded fonts.
# No product engine, SDK download or runtime font installation is added here.
function(cell_add_native_core target source_root)
    target_sources(${target} PRIVATE
        "${source_root}/src/midi_message.cpp" "${source_root}/src/smf.cpp"
        "${source_root}/src/gog.cpp" "${source_root}/src/tci.cpp" "${source_root}/src/trg.cpp"
        "${source_root}/src/text.cpp" "${source_root}/src/localization.cpp"
        "${source_root}/src/uuid.cpp" "${source_root}/src/file.cpp"
        "${source_root}/src/file_streams.cpp" "${source_root}/src/pooled_file.cpp" "${source_root}/src/dispatch.cpp"
        "${source_root}/src/action_history.cpp" "${source_root}/src/process.cpp")
    if(CMAKE_SYSTEM_NAME STREQUAL "iOS")
        target_compile_definitions(${target} PRIVATE CELL_NO_CHILD_PROCESS=1)
    endif()
    if(WIN32)
        target_link_libraries(${target} PRIVATE bcrypt shell32 ole32)
    else()
        target_link_libraries(${target} PRIVATE ${CMAKE_DL_LIBS})
        target_compile_definitions(${target} PRIVATE _FILE_OFFSET_BITS=64)
    endif()
endfunction()

# The independent SDK and product lab use one native file/codec component.
# Optional libsndfile is loaded dynamically; PCM/float WAV has no provider.
function(cell_add_audio_files source_root)
    add_library(cell_audio_files STATIC
        "${source_root}/src/audio_file.cpp"
        "${source_root}/src/wave_file.cpp"
        "${source_root}/src/audio_file_codecs.cpp")
    add_library(Cell::AudioFiles ALIAS cell_audio_files)
    set_target_properties(cell_audio_files PROPERTIES EXPORT_NAME AudioFiles
        POSITION_INDEPENDENT_CODE ON CXX_VISIBILITY_PRESET hidden
        VISIBILITY_INLINES_HIDDEN ON)
    cell_apply_compiler_contract(cell_audio_files)
    target_link_libraries(cell_audio_files PUBLIC cell_core PRIVATE ${CMAKE_DL_LIBS})
endfunction()

function(cell_add_native_ui target source_root)
    target_sources(${target} PRIVATE
        "${source_root}/src/ui/timeline.cpp" "${source_root}/src/ui/widget_drawing.cpp"
        "${source_root}/src/ui/widget_input.cpp" "${source_root}/src/ui/widget_component.cpp"
        "${source_root}/src/ui/widget_controls.cpp" "${source_root}/src/ui/widget_navigation.cpp"
        "${source_root}/src/ui/widget_services.cpp" "${source_root}/src/ui/file_dialog.cpp")
    if(APPLE)
        target_sources(${target} PRIVATE "${source_root}/src/ui/file_dialog_macos.mm")
        set_source_files_properties("${source_root}/src/ui/file_dialog_macos.mm" PROPERTIES COMPILE_FLAGS "-fobjc-arc")
    endif()
    target_include_directories(${target} PRIVATE "${CMAKE_CURRENT_BINARY_DIR}/generated")
    foreach(spec IN ITEMS
        "Inter.ttf|cell_font.hpp|font" "Inter-Bold.ttf|cell_font_bold.hpp|fontBold"
        "DejaVuSansMono.ttf|cell_font_mono.hpp|fontMono"
        "CellBrandGrotesk-SemiBold.ttf|cell_font_brand_grotesk.hpp|fontBrandGrotesk"
        "Rajdhani-SemiBold.ttf|cell_font_brand_condensed.hpp|fontBrandCondensed")
        string(REPLACE "|" ";" parts "${spec}")
        list(GET parts 0 input)
        list(GET parts 1 output)
        list(GET parts 2 symbol)
        set(generated "${CMAKE_CURRENT_BINARY_DIR}/generated/${output}")
        add_custom_command(OUTPUT "${generated}"
            COMMAND "${Python3_EXECUTABLE}" "${source_root}/tools/embed_resource.py"
                "${source_root}/assets/${input}" "${generated}" --symbol "${symbol}"
            DEPENDS "${source_root}/assets/${input}" "${source_root}/tools/embed_resource.py" VERBATIM)
        target_sources(${target} PRIVATE "${generated}")
    endforeach()
endfunction()
