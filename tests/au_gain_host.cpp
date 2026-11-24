// Original Cell Framework implementation; see LICENSE.
#include <AudioToolbox/AudioToolbox.h>
#include <cell/core.hpp>

#include <array>
#include <cmath>
#include <cstddef>
#include <iostream>
#include <stdexcept>
#include <string>

#ifndef CELL_TEST_AU_SUBTYPE
#define CELL_TEST_AU_SUBTYPE 0x4347616e // CGan
#endif
#ifndef CELL_TEST_AU_MANUFACTURER
#define CELL_TEST_AU_MANUFACTURER 0x43656c6c // Cell
#endif

namespace {
constexpr AudioComponentDescription description{0x61756678, CELL_TEST_AU_SUBTYPE, CELL_TEST_AU_MANUFACTURER, 0, 0};
constexpr auto gain = cell::stableId("gain");
constexpr UInt32 frames = 128;
constexpr double sampleRate = 48000;

void require(bool condition, const char* message) {
    if (!condition) throw std::runtime_error(message);
}

void check(OSStatus status, const char* operation) {
    if (status != noErr)
        throw std::runtime_error(std::string(operation) + " failed: " + std::to_string(status));
}

struct Instance {
    explicit Instance(AudioComponent component) { check(AudioComponentInstanceNew(component, &unit), "Open"); }
    ~Instance() { AudioComponentInstanceDispose(unit); }
    Instance(const Instance&) = delete;
    Instance& operator=(const Instance&) = delete;
    AudioUnit unit{};
};

struct State {
    CFPropertyListRef value{};
    ~State() { if (value) CFRelease(value); }
};

float source(UInt32 channel) { return channel == 0 ? 0.25f : -0.5f; }

OSStatus input(void*, AudioUnitRenderActionFlags*, const AudioTimeStamp*, UInt32, UInt32 count,
               AudioBufferList* buffers) {
    for (UInt32 channel = 0; channel < buffers->mNumberBuffers; ++channel) {
        auto& buffer = buffers->mBuffers[channel];
        if (!buffer.mData || buffer.mDataByteSize < count * sizeof(float))
            return kAudioUnitErr_InvalidPropertyValue;
        std::fill_n(static_cast<float*>(buffer.mData), count, source(channel));
    }
    return noErr;
}

void configure(AudioUnit unit, UInt32 channels) {
    const AudioStreamBasicDescription format{sampleRate, kAudioFormatLinearPCM,
        static_cast<AudioFormatFlags>(kAudioFormatFlagsNativeFloatPacked) | kAudioFormatFlagIsNonInterleaved,
        sizeof(float), 1, sizeof(float), channels, 32, 0};
    for (const auto scope : {kAudioUnitScope_Input, kAudioUnitScope_Output})
        check(AudioUnitSetProperty(unit, kAudioUnitProperty_StreamFormat, scope, 0, &format, sizeof(format)),
              "Set stream format");
    const AURenderCallbackStruct callback{input, nullptr};
    check(AudioUnitSetProperty(unit, kAudioUnitProperty_SetRenderCallback, kAudioUnitScope_Input, 0,
                               &callback, sizeof(callback)), "Connect input");
    check(AudioUnitInitialize(unit), "Initialize");
}

void expectRender(AudioUnit unit, UInt32 channels, double& position, double decibels, int warmupBlocks = 0) {
    struct Buffers {
        UInt32 mNumberBuffers;
        AudioBuffer mBuffers[2];
    };
    static_assert(offsetof(Buffers, mBuffers) == offsetof(AudioBufferList, mBuffers));
    std::array<std::array<float, frames>, 2> audio{};
    Buffers buffers{channels, {}};
    for (UInt32 channel = 0; channel < channels; ++channel)
        buffers.mBuffers[channel] = {1, sizeof(audio[channel]), audio[channel].data()};
    for (int i = 0; i <= warmupBlocks; ++i) {
        AudioTimeStamp time{};
        time.mSampleTime = position;
        time.mFlags = kAudioTimeStampSampleTimeValid;
        AudioUnitRenderActionFlags flags{};
        check(AudioUnitRender(unit, &flags, &time, 0, frames, reinterpret_cast<AudioBufferList*>(&buffers)),
              "Render");
        require((flags & kAudioUnitRenderAction_OutputIsSilence) == 0, "Output marked silent");
        position += frames;
    }
    const auto level = std::pow(10.0, decibels / 20.0);
    for (UInt32 channel = 0; channel < channels; ++channel)
        for (auto sample : audio[channel])
            require(std::isfinite(sample) && std::abs(sample - source(channel) * level) < 1e-6,
                    "Rendered gain or channel routing differs");
}

void setGain(AudioUnit unit, float level) {
    check(AudioUnitSetParameter(unit, gain, kAudioUnitScope_Global, 0, level, 0), "Set gain");
}
}

int main() {
    try {
        auto component = AudioComponentFindNext(nullptr, &description);
        require(component != nullptr, "The test Audio Unit is not registered");
        Instance original(component);
        AudioUnitParameterInfo info{};
        UInt32 size = sizeof(info);
        check(AudioUnitGetProperty(original.unit, kAudioUnitProperty_ParameterInfo, kAudioUnitScope_Global,
                                   gain, &info, &size), "Get gain metadata");
        if (info.cfNameString && (info.flags & kAudioUnitParameterFlag_CFNameRelease)) CFRelease(info.cfNameString);
        require(info.unit == kAudioUnitParameterUnit_Decibels && info.minValue == -60 && info.maxValue == 12,
                "Unexpected gain metadata");
        setGain(original.unit, -6);
        State state;
        size = sizeof(state.value);
        check(AudioUnitGetProperty(original.unit, kAudioUnitProperty_ClassInfo, kAudioUnitScope_Global,
                                   0, &state.value, &size), "Save AU state");
        require(state.value != nullptr, "Missing saved state");
        for (UInt32 channels : {1u, 2u}) {
            Instance reopened(component);
            check(AudioUnitSetProperty(reopened.unit, kAudioUnitProperty_ClassInfo, kAudioUnitScope_Global,
                                       0, &state.value, sizeof(state.value)), "Reopen AU state");
            configure(reopened.unit, channels);
            double position{};
            expectRender(reopened.unit, channels, position, -6);
            CFPropertyListRef invalid = CFSTR("invalid state");
            require(AudioUnitSetProperty(reopened.unit, kAudioUnitProperty_ClassInfo, kAudioUnitScope_Global,
                                         0, &invalid, sizeof(invalid)) != noErr, "Invalid state was accepted");
            expectRender(reopened.unit, channels, position, -6);
            setGain(reopened.unit, 3);
            expectRender(reopened.unit, channels, position, 3, 8);
        }
        std::cout << "Cell AU discovery, state recall, mono/stereo render and parameter changes passed.\n";
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
