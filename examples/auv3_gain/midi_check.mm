// Original Cell Framework implementation; see LICENSE.
#include "midi_fixture.hpp"
#import <AudioToolbox/AUAudioUnitImplementation.h>
#import <AVFoundation/AVFoundation.h>
#include <algorithm>
#include <array>

namespace cell::auv3_example {
namespace {
struct Event {
    AUEventSampleTime at{};
    std::array<std::uint8_t, 3> data{};
};

struct Capture {
    std::array<Event, 64> events{};
    std::size_t count{};
    std::array<std::uint8_t, 9> sysEx{};
    std::size_t sysExCount{};
    AUEventSampleTime sysExTime{};
};

NSString* verifyRender(AUAudioUnit* unit, AURenderBlock render, AUScheduleMIDIEventBlock schedule,
                       AUScheduleParameterBlock scheduleParameter, Capture& capture, bool checkEmptyClockBus) {
    constexpr AUEventSampleTime start = 1024;
    constexpr std::array<std::uint8_t, 3> first{0x90, 60, 100}, second{0x90, 62, 95};
    constexpr std::array<std::uint8_t, 9> sysEx{0xf0, 0x7d, 1, 2, 3, 4, 5, 6, 0xf7};
    schedule(start + 4, 0, first.size(), first.data());
    scheduleParameter(start + 16, 0, transpose, 7);
    schedule(start + 18, 0, sysEx.size(), sysEx.data());
    schedule(start + 20, 0, second.size(), second.data());
    std::array<float, midiFrames> dummy;
    dummy.fill(1.f);
    AudioBufferList output{1, {{1, sizeof(dummy), dummy.data()}}};
    AudioUnitRenderActionFlags flags{};
    AudioTimeStamp stamp{};
    stamp.mFlags = kAudioTimeStampSampleTimeValid;
    stamp.mSampleTime = start;
    auto status = render(&flags, &stamp, midiFrames, 0, &output, nil);
    if (status != noErr) return [NSString stringWithFormat:@"MIDI effect render failed (%d)", static_cast<int>(status)];
    if (!(flags & kAudioUnitRenderAction_OutputIsSilence) ||
        !std::all_of(dummy.begin(), dummy.end(), [](float sample) { return sample == 0.f; }))
        return @"MIDI effect did not produce silence on its clock bus";
    if (capture.count != 2 || capture.events[0].at != start + 4 || capture.events[0].data != first ||
        capture.events[1].at != start + 20 || capture.events[1].data != std::array<std::uint8_t, 3>{0x90, 69, 95})
        return @"MIDI effect did not preserve note timing across the scheduled transpose edit";
    if (capture.sysExCount != 1 || capture.sysExTime != start + 18 || capture.sysEx != sysEx)
        return @"MIDI effect did not preserve SysEx bytes and sample time";

    capture.count = 0;
    flags = 0;
    stamp.mSampleTime += midiFrames;
    if (checkEmptyClockBus) {
        AudioBufferList noBuffers{};
        status = unit.internalRenderBlock(&flags, &stamp, midiFrames, 0, &noBuffers, nullptr, nil);
    } else {
        status = render(&flags, &stamp, midiFrames, 0, &output, nil);
    }
    if (status != noErr || capture.count != 1 || capture.events[0].at != start + midiFrames ||
        capture.events[0].data != std::array<std::uint8_t, 3>{0xb0, 7, 100})
        return @"MIDI effect did not generate a timed event without incoming MIDI";
    return nil;
}
} // namespace

NSString* verifyMidiEffect(AUAudioUnit* unit, bool checkEmptyClockBus) {
    if (unit.inputBusses.count != 0 || unit.outputBusses.count != 1 ||
        unit.virtualMIDICableCount != 1 || unit.MIDIOutputNames.count != 1)
        return @"Incorrect MIDI effect bus metadata";
    const auto schedule = unit.scheduleMIDIEventBlock;
    const auto scheduleParameter = unit.scheduleParameterBlock;
    const auto render = unit.renderBlock;
    if (!schedule || !scheduleParameter || !render)
        return @"MIDI effect did not publish its render and scheduling API";

    NSError* error = nil;
    AVAudioFormat* clockFormat = [[AVAudioFormat alloc] initStandardFormatWithSampleRate:midiSampleRate channels:1];
    if (![unit.outputBusses[0] setFormat:clockFormat error:&error])
        return error.localizedDescription ?: @"MIDI effect clock bus rejected mono";
    unit.maximumFramesToRender = midiFrames;
    unit.renderingOffline = YES;
    Capture capture;
    auto* sink = &capture;
    unit.MIDIOutputEventBlock = ^OSStatus(AUEventSampleTime at, std::uint8_t cable, NSInteger size,
                                          const std::uint8_t* data) {
        if (cable || !data || size < 1) return kAudioUnitErr_CannotDoInCurrentContext;
        if (data[0] == 0xf0) {
            if (size != NSInteger(sink->sysEx.size())) return kAudioUnitErr_CannotDoInCurrentContext;
            std::copy_n(data, sink->sysEx.size(), sink->sysEx.begin());
            sink->sysExTime = at;
            ++sink->sysExCount;
            return noErr;
        }
        if (size != 3 || sink->count == sink->events.size()) return kAudioUnitErr_CannotDoInCurrentContext;
        sink->events[sink->count++] = {at, {data[0], data[1], data[2]}};
        return noErr;
    };
    NSString* failure;
    if ([unit allocateRenderResourcesAndReturnError:&error]) {
        failure = verifyRender(unit, render, schedule, scheduleParameter, capture, checkEmptyClockBus);
        [unit deallocateRenderResources];
    } else {
        failure = error.localizedDescription ?: @"MIDI effect could not allocate render resources";
    }
    unit.MIDIOutputEventBlock = nil;
    return failure;
}
} // namespace cell::auv3_example
