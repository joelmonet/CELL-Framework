include_guard(GLOBAL)

# Every Cell consumer sees the actual language version. MSVC otherwise reports
# C++98 through __cplusplus even when /std:c++20 is selected, causing libraries
# such as the VST3 SDK to compile the wrong filesystem/UTF-8 implementation.
function(cell_apply_compiler_contract target)
    target_compile_features(${target} PUBLIC cxx_std_20)
    target_compile_options(${target} PUBLIC
        "$<$<AND:$<COMPILE_LANGUAGE:CXX>,$<CXX_COMPILER_ID:MSVC>>:/Zc:__cplusplus>"
        "$<$<AND:$<COMPILE_LANGUAGE:CXX>,$<CXX_COMPILER_ID:MSVC>>:/utf-8>")
endfunction()
