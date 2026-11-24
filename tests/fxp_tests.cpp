// SPDX-License-Identifier: Apache-2.0
#include <cell/fxp.hpp>
#include <iostream>

int main(){try{
    constexpr auto id=0x48765262u;
    const std::string state="<HSVerbRack><Module slot='17' type='32'/></HSVerbRack>";
    const auto payload=std::as_bytes(std::span(state));
    auto file=cell::presets::writeFxp(id,405,"A complete rack",payload);
    const auto read=cell::presets::readFxp(file,id);
    if(read.name!="A complete rack"||read.pluginVersion!=405||!std::equal(read.chunk.begin(),read.chunk.end(),payload.begin(),payload.end()))throw std::runtime_error("FXP round trip changed the opaque rack state");
    const auto reject=[&](std::span<const std::byte> bytes,std::uint32_t plugin=0x48765262u,std::size_t limit=128u*1024*1024){try{(void)cell::presets::readFxp(bytes,plugin,limit);}catch(const std::invalid_argument&){return;}throw std::runtime_error("Malformed or foreign FXP was accepted");};
    for(std::size_t size=0;size<file.size();++size)reject(std::span(file).first(size));
    reject(file,id^1);reject(file,id,payload.size()-1);
    for(unsigned offset:{0u,4u,8u,12u,24u,56u}){auto corrupt=file;corrupt[offset]^=std::byte{1};reject(corrupt);}
    auto extra=file;extra.push_back(std::byte{});reject(extra);
    auto unicode=cell::presets::readFxp(cell::presets::writeFxp(id,1,std::string(26,'A')+"é",payload),id);
    if(unicode.name!=std::string(26,'A'))throw std::runtime_error("FXP program name split a UTF-8 character");
    std::cout<<"PASS FXP chunk framing, full opaque state, foreign-plugin checks, bounded reads, truncation and UTF-8 naming\n";
}catch(const std::exception& error){std::cerr<<error.what()<<'\n';return 1;}}
