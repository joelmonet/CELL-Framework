// SPDX-License-Identifier: Apache-2.0
#include <filesystem>
#include <string>
#include <type_traits>
#include <iostream>
#ifdef CELL_CHECK_VST3_COMPILER
#include "pluginterfaces/base/fplatform.h"
static_assert(SMTG_CPP20,"The VST3 SDK must select its C++20 implementation");
#endif
static_assert(__cplusplus>=202002L,"The compiler must report the selected C++20 language version");
static_assert(__cpp_char8_t>=201811L,"Do not disable native char8_t support");
static_assert(std::is_same_v<decltype(std::filesystem::path{}.generic_u8string()),std::u8string>);
int main(){
    const std::u8string expected=u8"Sound C\u00e8lls/\u30df\u30c3\u30af\u30b9.vst3";
    const std::filesystem::path path(expected);const auto utf8=path.generic_u8string();
    // This is the SDK's C++20 conversion: copy UTF-8 code units without using
    // the active Windows ANSI code page or treating UTF-16 bytes as UTF-8.
    const std::string bytes(utf8.begin(),utf8.end());
    const std::string expectedBytes="Sound C\xc3\xa8lls/\xe3\x83\x9f\xe3\x83\x83\xe3\x82\xaf\xe3\x82\xb9.vst3";
    if(bytes!=expectedBytes){std::cerr<<"UTF-8 plugin path changed during conversion\n";return 1;}
    std::cout<<"PASS: accurate C++20 reporting, native char8_t, filesystem UTF-8 path conversion\n";
}
