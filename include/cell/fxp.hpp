// SPDX-License-Identifier: Apache-2.0
#pragma once
#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

namespace cell::presets {
// VST chunk-program FXP framing. The chunk remains owned and interpreted by
// its plugin; this does not translate presets from unrelated instruments.
struct ChunkProgram {
    std::uint32_t pluginId{},pluginVersion{};
    std::string name;
    std::vector<std::byte> chunk;
};
inline ChunkProgram readFxp(std::span<const std::byte> bytes,std::uint32_t expectedPlugin,
                            std::size_t maximumChunk=128u*1024*1024){
    const auto word=[&](std::size_t at){std::uint32_t value=0;for(unsigned n=0;n<4;++n)value=(value<<8)|std::to_integer<unsigned>(bytes[at+n]);return value;};
    if(bytes.size()<60||word(0)!=0x43636e4b||word(8)!=0x46504368)
        throw std::invalid_argument("Choose a chunk-program .fxp preset");
    if(word(4)!=bytes.size()-8||word(12)!=1||word(24)!=1||word(56)!=bytes.size()-60||word(56)>maximumChunk)
        throw std::invalid_argument("Invalid or incomplete .fxp preset");
    if(word(16)!=expectedPlugin)throw std::invalid_argument("This .fxp belongs to a different plugin");
    ChunkProgram result{word(16),word(20),{}, {bytes.begin()+60,bytes.end()}};
    for(std::size_t n=28;n<56&&bytes[n]!=std::byte{};++n)result.name+=char(std::to_integer<unsigned char>(bytes[n]));
    return result;
}
inline std::vector<std::byte> writeFxp(std::uint32_t pluginId,std::uint32_t pluginVersion,
                                      std::string_view name,std::span<const std::byte> chunk){
    if(chunk.size()>128u*1024*1024)throw std::length_error("Preset chunk is too large");
    std::vector<std::byte> bytes(60+chunk.size());
    const auto word=[&](std::size_t at,std::uint32_t value){for(unsigned n=0;n<4;++n)bytes[at+n]=std::byte((value>>(24-8*n))&255u);};
    word(0,0x43636e4b);word(4,std::uint32_t(bytes.size()-8));word(8,0x46504368);word(12,1);
    word(16,pluginId);word(20,pluginVersion);word(24,1);word(56,std::uint32_t(chunk.size()));
    auto length=std::min(name.size(),std::size_t(27));
    while(length<name.size()&&length&&(static_cast<unsigned char>(name[length])&0xc0)==0x80)--length;
    for(std::size_t n=0;n<length;++n)bytes[28+n]=std::byte(static_cast<unsigned char>(name[n]));
    std::copy(chunk.begin(),chunk.end(),bytes.begin()+60);return bytes;
}
}
