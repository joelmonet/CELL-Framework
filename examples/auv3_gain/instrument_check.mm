// Original Cell Framework implementation; see LICENSE.
#include "instrument_fixture.hpp"
#import <AVFoundation/AVFoundation.h>
#include <array>
#include <cmath>
#include <numbers>

namespace cell::auv3_example {
namespace {
NSString* verifyTone(AUAudioUnit* unit, UInt32 channels, AURenderBlock render,
                     AUScheduleMIDIEventBlock schedule, AUScheduleParameterBlock scheduleParameter) {
    struct Buffers {
        UInt32 count;
        AudioBuffer audio[2];
    };
    static_assert(offsetof(Buffers, audio) == offsetof(AudioBufferList, mBuffers));
    std::array<std::array<float, instrumentFrames>, 2> samples{};
    Buffers output{channels, {{1, sizeof(samples[0]), samples[0].data()},
                             {1, sizeof(samples[1]), samples[1].data()}}};
    AudioTimeStamp time{};
    time.mSampleTime = 1024;
    time.mFlags = kAudioTimeStampSampleTimeValid;
    AudioUnitRenderActionFlags flags{};
    const auto renderBlock = [&] {
        flags = 0;
        for (auto& data : samples) data.fill(-1.f);
        const auto status = render(&flags, &time, instrumentFrames, 0, reinterpret_cast<AudioBufferList*>(&output), nil);
        time.mSampleTime += instrumentFrames;
        return status;
    };
    constexpr std::array<std::uint8_t, 3> noteOn{0x90, 69, 127}, noteOff{0x80, 69, 0};
    constexpr UInt32 onFrame = 5, levelFrame = 20, offFrame = 45;
    auto start = static_cast<AUEventSampleTime>(time.mSampleTime);
    schedule(start + onFrame, 0, noteOn.size(), noteOn.data());
    scheduleParameter(start + levelFrame, 0, instrumentLevel, 0.5);
    schedule(start + offFrame, 0, noteOff.size(), noteOff.data());
    if (const auto status = renderBlock(); status != noErr)
        return [NSString stringWithFormat:@"Instrument render failed (%d)", static_cast<int>(status)];
    if (flags & kAudioUnitRenderAction_OutputIsSilence) return @"Instrument marked a sounding render as silent";
    for (UInt32 channel = 0; channel < channels; ++channel) {
        for (UInt32 frame = 0; frame < instrumentFrames; ++frame) {
            double expected{};
            if (onFrame <= frame && frame < offFrame) {
                const auto level = frame < levelFrame ? 0.25 : 0.5;
                expected = level * std::cos(2 * std::numbers::pi * 440 * (frame - onFrame) / instrumentRate);
            }
            if (!std::isfinite(samples[channel][frame]) || std::abs(samples[channel][frame] - expected) > 1e-6)
                return [NSString stringWithFormat:@"Instrument note or level timing differs at frame %u on channel %u",
                    frame, channel];
        }
    }

    start = static_cast<AUEventSampleTime>(time.mSampleTime);
    schedule(start + onFrame, 0, noteOn.size(), noteOn.data());
    if (renderBlock() != noErr || !(std::abs(samples[0][onFrame] - 0.5f) < 1e-6f))
        return @"Instrument did not start a held note for the bypass check";

    start = static_cast<AUEventSampleTime>(time.mSampleTime);
    schedule(start + offFrame, 0, noteOff.size(), noteOff.data());
    unit.shouldBypassEffect = YES;
    const auto bypassStatus = renderBlock();
    unit.shouldBypassEffect = NO;
    if (bypassStatus != noErr || !(flags & kAudioUnitRenderAction_OutputIsSilence))
        return @"Instrument bypass did not mute audio";
    if (renderBlock() != noErr) return @"Instrument render failed after bypass";
    if (!(flags & kAudioUnitRenderAction_OutputIsSilence)) {
        for (UInt32 channel = 0; channel < channels; ++channel)
            for (auto sample : samples[channel])
                if (sample != 0.f) return @"Instrument kept a note that was released during bypass";
    }
    return nil;
}
} // namespace

NSString* verifyInstrument(AUAudioUnit* unit) {
    if (unit.inputBusses.count != 0 || unit.outputBusses.count != 1 || unit.virtualMIDICableCount != 1 ||
        ![unit.channelCapabilities isEqualToArray:@[@0, @2, @0, @1]])
        return @"Incorrect instrument bus capabilities";
    auto* level = [unit.parameterTree parameterWithAddress:instrumentLevel];
    const auto render = unit.renderBlock;
    const auto schedule = unit.scheduleMIDIEventBlock;
    const auto scheduleParameter = unit.scheduleParameterBlock;
    if (!level || !render || !schedule || !scheduleParameter) return @"Instrument did not expose scheduling and rendering";

    unit.maximumFramesToRender = instrumentFrames;
    unit.renderingOffline = YES;
    for (UInt32 channels : {2u, 1u}) {
        NSError* error = nil;
        AVAudioFormat* format = [[AVAudioFormat alloc] initStandardFormatWithSampleRate:instrumentRate channels:channels];
        if (![unit.outputBusses[0] setFormat:format error:&error])
            return error.localizedDescription ?: @"Instrument output format rejected";
        level.value = 0.25;
        if (![unit allocateRenderResourcesAndReturnError:&error])
            return error.localizedDescription ?: @"Instrument render allocation failed";
        NSString* failure = verifyTone(unit, channels, render, schedule, scheduleParameter);
        [unit deallocateRenderResources];
        if (failure) return failure;
    }
    return nil;
}
} // namespace cell::auv3_example
