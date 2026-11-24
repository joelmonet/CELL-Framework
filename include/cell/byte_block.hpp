// Original Cell Framework implementation; see LICENSE.
#pragma once
#include <cell/legacy_state.hpp>
#include <cell/text.hpp>
#include <cstring>
#include <span>
#include <vector>

namespace cell {
class ByteBlock {
    std::vector<std::byte> bytes_;
public:
    ByteBlock()=default;
    explicit ByteBlock(std::size_t size,bool=true):bytes_(size){}
    ByteBlock(const void* data,std::size_t size):bytes_(size){if(size){if(!data)throw std::invalid_argument("Binary source is null");std::memcpy(bytes_.data(),data,size);}}
    explicit ByteBlock(std::vector<std::byte> bytes):bytes_(std::move(bytes)){}
    void* getData() noexcept{return bytes_.data();}
    const void* getData() const noexcept{return bytes_.data();}
    std::size_t getSize() const noexcept{return bytes_.size();}
    void setSize(std::size_t size,bool=true){bytes_.resize(size);}
    std::span<const std::byte> bytes() const noexcept{return bytes_;}
    void append(const void*source,std::size_t count){if(!count)return;if(!source||count>bytes_.max_size()-bytes_.size())throw std::length_error("Binary append overflow");const auto p=reinterpret_cast<std::uintptr_t>(source),base=reinterpret_cast<std::uintptr_t>(bytes_.data());const bool alias=!bytes_.empty()&&p>=base&&p-base<bytes_.size();const auto offset=alias?std::size_t(p-base):0;if(alias&&count>bytes_.size()-offset)throw std::out_of_range("Binary append source");const auto at=bytes_.size();bytes_.resize(at+count);std::memmove(bytes_.data()+at,alias?bytes_.data()+offset:source,count);}
    void reset() noexcept{bytes_.clear();}
    bool operator==(const ByteBlock&)const=default;
    // Compatibility spelling uses the legacy length-prefixed six-bit wire
    // encoding. It is deliberately distinct from RFC 4648 base64.
    Text toBase64Encoding() const{return Text(legacy::encodeMemoryBlock(bytes_,bytes_.size()));}
    bool fromBase64Encoding(const Text& text){try{auto decoded=legacy::decodeMemoryBlock(text.utf8(),text.getNumBytesAsUTF8());bytes_=std::move(decoded);return true;}catch(const legacy::ReadError&){return false;}}
};
} // namespace cell
