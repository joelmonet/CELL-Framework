// SPDX-License-Identifier: Apache-2.0
#pragma once
#include <cell/audio.hpp>
#include <algorithm>
#include <cstring>
#include <stdexcept>

namespace cell::midi {
inline constexpr std::size_t eventCapacity=4096;
inline constexpr std::size_t sysExByteCapacity = 256u * 1024;

inline bool validSysEx(std::span<const std::uint8_t> bytes) noexcept {
    return bytes.size() >= 2 && bytes.size() <= sysExByteCapacity &&
        bytes.front() == 0xf0 && bytes.back() == 0xf7 &&
        std::all_of(bytes.begin() + 1, bytes.end() - 1, [](auto byte) { return byte < 0x80; });
}

class SysExBudget {
public:
    void clear() noexcept { used_ = 0; }
    bool admit(std::span<const std::uint8_t> bytes) noexcept {
        if (bytes.size() > sysExByteCapacity - used_) return false;
        // Charge validation work for malformed packets too.
        used_ += bytes.size();
        return validSysEx(bytes);
    }
private:
    std::size_t used_{};
};

// Own variable-length payloads in sinks whose output outlives a push() call.
class SysExStorage {
public:
    void prepare(std::size_t capacity) {
        if (capacity > sysExByteCapacity) throw std::invalid_argument("Invalid SysEx buffer capacity");
        bytes_.resize(capacity);
        clear();
    }
    void clear() noexcept { used_ = 0; }
    std::size_t capacity() const noexcept { return bytes_.size(); }
    std::span<const std::uint8_t> copy(std::span<const std::uint8_t> bytes) noexcept {
        if (bytes.empty() || bytes.size() > capacity() - used_) return {};
        auto* destination = bytes_.data() + used_;
        std::memmove(destination, bytes.data(), bytes.size());
        used_ += bytes.size();
        return {destination, bytes.size()};
    }
private:
    std::vector<std::uint8_t> bytes_;
    std::size_t used_{};
};

class Output {
public:
    virtual ~Output()=default;
    virtual bool push(MidiEvent)noexcept=0;
    virtual void reject()noexcept{}
};
inline void forward(std::span<const MidiEvent> input, Output& output,
                    std::uint32_t frames = UINT32_MAX) noexcept {
    for (const auto& event : input)
        if (event.frame < frames && !output.push(event) && !event.isSysEx()) break;
}
// Allocate on preparation only. An overflow is visible and can be replaced by
// a complete panic batch, rather than forwarding a partly admitted chord.
class Buffer final:public Output {
public:
    Buffer() = default;
    Buffer(const Buffer& other) : storage_(other.storage_.size()) {
        sysEx_.prepare(other.sysEx_.capacity());
        for (const auto& event : other.events()) push(event);
        overflow_ = other.overflow_;
    }
    Buffer(Buffer&& other) noexcept { swap(other); }
    Buffer& operator=(Buffer other) noexcept { swap(other); return *this; }
    void prepare(std::size_t capacity=eventCapacity,std::size_t sysExBytes=0) {
        if(capacity<48||capacity>eventCapacity)throw std::invalid_argument("Invalid MIDI event capacity");
        storage_.resize(capacity);sysEx_.prepare(sysExBytes);clear();
    }
    void clear()noexcept{size_=0;overflow_=false;sysEx_.clear();}
    bool push(MidiEvent event)noexcept override {
        if(size_==storage_.size()){overflow_=true;return false;}
        if(event.isSysEx()){
            if(!validSysEx(event.sysEx)){overflow_=true;return false;}
            event.sysEx=sysEx_.copy(event.sysEx);
            if(event.sysEx.empty()){overflow_=true;return false;}
        }
        storage_[size_++]=event;return true;
    }
    std::span<const MidiEvent> events()const noexcept{return {storage_.data(),size_};}
    bool overflowed()const noexcept{return overflow_;}
    void reject()noexcept override{overflow_=true;}
    std::size_t storageBytes()const noexcept{return storage_.size()*sizeof(MidiEvent)+sysEx_.capacity();}
    void emergency(std::uint32_t frame=0)noexcept{
        clear();overflow_=true;
        for(unsigned ch=0;ch<16;++ch)for(unsigned cc:{64u,120u,123u})if(size_<storage_.size())storage_[size_++]={frame,{std::uint8_t(0xb0|ch),std::uint8_t(cc),0},3};
    }
private:
    void swap(Buffer& other) noexcept {
        storage_.swap(other.storage_);
        std::swap(sysEx_, other.sysEx_);
        std::swap(size_, other.size_);
        std::swap(overflow_, other.overflow_);
    }
    std::vector<MidiEvent> storage_;SysExStorage sysEx_;std::size_t size_{};bool overflow_{};
};
inline bool valid(const MidiEvent& e)noexcept {
    const unsigned status=e.data[0],kind=status&0xf0;
    if(e.isSysEx()||status<0x80||status>=0xf0||e.data[1]>127)return false;
    if(kind==0xc0||kind==0xd0)return e.size==2||e.size==3;
    return e.size==3&&e.data[2]<128;
}
}
