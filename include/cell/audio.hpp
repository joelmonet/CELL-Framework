#pragma once

#include <cell/owned_audio.hpp>
#include <algorithm>
#include <array>
#include <cassert>
#include <cmath>
#include <cstdint>
#include <span>
#include <stdexcept>
#include <utility>
#include <vector>

namespace cell {

struct ProcessSpec {
    double sampleRate = 48000.0;
    std::uint32_t maxFrames = 512;
    std::uint32_t channels = 2;
};

template<class Sample>
struct AudioBlock {
    Sample* const* channels{};
    std::uint32_t channelCount{};
    std::uint32_t frames{};
    std::uint32_t offset{};

    Sample* channel(std::uint32_t index) const noexcept {
        assert(index < channelCount);
        return channels[index] + offset;
    }
    AudioBlock slice(std::uint32_t start, std::uint32_t count) const noexcept {
        assert(start <= frames && count <= frames - start);
        return {channels, channelCount, count, offset + start};
    }
    void clear() const noexcept {
        for (std::uint32_t c = 0; c < channelCount; ++c)
            std::fill_n(channel(c), frames, Sample{});
    }
    void multiply(Sample gain) const noexcept {
        for (std::uint32_t c = 0; c < channelCount; ++c)
            for (std::uint32_t i = 0; i < frames; ++i) channel(c)[i] *= gain;
    }
};

template<class Sample=float>class AudioBuffer:public OwnedAudio<Sample>{
 public:using OwnedAudio<Sample>::OwnedAudio;
 AudioBlock<Sample>block(){if(this->channelCount()>std::numeric_limits<std::uint32_t>::max()||this->frameCount()>std::numeric_limits<std::uint32_t>::max())throw std::length_error("Use a wide view to slice this buffer into processing blocks");return {this->getArrayOfWritePointers(),std::uint32_t(this->channelCount()),std::uint32_t(this->frameCount()),0};}
};

struct Transport {
    bool playing{};
    bool recording{};
    bool looping{};
    double tempo = 120.0;
    double beatPosition{};
    std::int64_t samplePosition{};
    std::uint16_t numerator = 4;
    std::uint16_t denominator = 4;
    bool offline{};
    // A real sample clock is distinct from its default numerical value of 0.
    bool samplePositionValid{};
    // A usable tempo is separate from its numerical fallback. Legacy C++
    // producers retain their existing default; host adapters always set this
    // explicitly from the host's availability flags and a finite positive BPM.
    bool tempoValid = true;
    bool loopPointsValid{};
    double loopStartBeat{},loopEndBeat{};
    // A supplied quarter-note position, including zero or negative pre-roll.
    bool beatPositionValid{};
};

struct MidiEvent {
    std::uint32_t frame{};
    std::array<std::uint8_t, 3> data{};
    std::uint8_t size = 3;
};

inline float dbToGain(float db) noexcept { return std::pow(10.0f, db * 0.05f); }
inline float gainToDb(float gain, float floor = -120.0f) noexcept {
    return gain > 0 ? std::max(floor, 20.0f * std::log10(gain)) : floor;
}

} // namespace cell
