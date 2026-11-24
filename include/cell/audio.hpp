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

constexpr std::uint8_t umpWordCount(std::uint32_t firstWord) noexcept {
    switch (firstWord >> 28) {
    case 0: case 1: case 2: return 1;
    case 3: case 4: return 2;
    case 5: case 0xd: case 0xf: return 4;
    default: return 0;
    }
}

struct MidiEvent {
    std::uint32_t frame{};
    std::array<std::uint8_t, 3> data{};
    std::uint8_t size = 3;
    // Complete MIDI 1.0 wire message, borrowed for the current process call.
    std::span<const std::uint8_t> sysEx{};
    // Host-order Universal MIDI Packet words. Inline storage survives block slicing and queues.
    std::array<std::uint32_t, 4> ump{};
    std::uint8_t umpCount{};
    // Optional host voice identity. MIDI wire transports address notes by group, channel and pitch.
    std::int32_t noteId{-1};

    bool isUmp() const noexcept { return umpCount != 0; }
    bool isSysEx() const noexcept { return !isUmp() && (data[0] == 0xf0 || !sysEx.empty()); }
    static MidiEvent systemExclusive(std::uint32_t frame, std::span<const std::uint8_t> bytes) noexcept {
        return {frame, {0xf0, 0, 0}, 0, bytes};
    }
    static MidiEvent universalPacket(std::uint32_t frame, std::span<const std::uint32_t> words) noexcept {
        MidiEvent event{frame, {}, 0};
        if (words.empty() || words.size() != umpWordCount(words.front())) return event;
        event.umpCount = static_cast<std::uint8_t>(words.size());
        for (std::size_t i = 0; i < words.size(); ++i) event.ump[i] = words[i];
        return event;
    }
};

inline float dbToGain(float db) noexcept { return std::pow(10.0f, db * 0.05f); }
inline float gainToDb(float gain, float floor = -120.0f) noexcept {
    return gain > 0 ? std::max(floor, 20.0f * std::log10(gain)) : floor;
}

} // namespace cell
