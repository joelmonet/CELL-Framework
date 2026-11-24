// SPDX-License-Identifier: Apache-2.0
#pragma once
#include <cell/audio.hpp>
#include <algorithm>
#include <stdexcept>

namespace cell::midi {
inline constexpr std::size_t eventCapacity=4096;
class Output {
public:
    virtual ~Output()=default;
    virtual bool push(MidiEvent)noexcept=0;
    virtual void reject()noexcept{}
};
// Allocate on preparation only. An overflow is visible and can be replaced by
// a complete panic batch, rather than forwarding a partly admitted chord.
class Buffer final:public Output {
public:
    void prepare(std::size_t capacity=eventCapacity){if(capacity<48||capacity>eventCapacity)throw std::invalid_argument("Invalid MIDI event capacity");storage_.resize(capacity);clear();}
    void clear()noexcept{size_=0;overflow_=false;}
    bool push(MidiEvent event)noexcept override{if(size_==storage_.size()){overflow_=true;return false;}storage_[size_++]=event;return true;}
    std::span<const MidiEvent> events()const noexcept{return {storage_.data(),size_};}
    bool overflowed()const noexcept{return overflow_;}
    void reject()noexcept override{overflow_=true;}
    std::size_t storageBytes()const noexcept{return storage_.size()*sizeof(MidiEvent);}
    void emergency(std::uint32_t frame=0)noexcept{
        size_=0;overflow_=true;
        for(unsigned ch=0;ch<16;++ch)for(unsigned cc:{64u,120u,123u})if(size_<storage_.size())storage_[size_++]={frame,{std::uint8_t(0xb0|ch),std::uint8_t(cc),0},3};
    }
private:
    std::vector<MidiEvent> storage_;std::size_t size_{};bool overflow_{};
};
inline bool valid(const MidiEvent& e)noexcept {
    const unsigned status=e.data[0],kind=status&0xf0;
    if(status<0x80||status>=0xf0||e.data[1]>127)return false;
    if(kind==0xc0||kind==0xd0)return e.size==2||e.size==3;
    return e.size==3&&e.data[2]<128;
}
}
