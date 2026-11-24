// SPDX-License-Identifier: Apache-2.0
#import <AudioToolbox/AudioToolbox.h>
#import <AudioToolbox/AUAudioUnitImplementation.h>
#import <AVFoundation/AVFoundation.h>
#import <CoreMIDI/CoreMIDI.h>
#include <cell/plugin.hpp>
#include <array>
#include <cmath>
#include <iostream>
#include <stdexcept>

@interface CellAUv3Effect : AUAudioUnit
@end

namespace {
constexpr AudioComponentDescription description{kAudioUnitType_MusicEffect, 0x43545374, 0x43656c6c, 0, 0};
constexpr UInt32 frames = 128, edit = 32;
constexpr AUEventSampleTime renderTime = 2048;
constexpr double sampleRate = 48000;
const std::array<cell::Parameter, 2> fields{{
    {17, "edit", "Edit", "", 0, 1, 0},
    {18, "mode", "Mode", "", 0, 1, 0, 1},
}};
using cell::AudioLayout;
constexpr std::array mainLayouts{
    cell::MainBusLayout{AudioLayout::mono, AudioLayout::mono},
    cell::MainBusLayout{AudioLayout::stereo, AudioLayout::stereo},
    cell::MainBusLayout{AudioLayout::surround51, AudioLayout::surround51},
    cell::MainBusLayout{AudioLayout::surround712, AudioLayout::surround712},
    cell::MainBusLayout{AudioLayout::surround714, AudioLayout::surround714},
    cell::MainBusLayout{AudioLayout::surround916, AudioLayout::surround916},
};
std::array<cell::Transport, 2> observed;
std::size_t observations{};
std::uint32_t observedSidechainChannels{};
struct MidiObservation {
    cell::MidiEvent event;
    double parameter{};
};
std::array<MidiObservation, 8> observedMidi;
std::size_t observedMidiCount{};
bool midiInRange{};
unsigned resets{};
bool generateMidi{};
bool markChannels{};
bool verifyUmpOnly{};

class Probe final : public cell::Processor {
public:
    void prepare(const cell::ProcessSpec&) override {}
    void reset() noexcept override { ++resets; }
    void process(cell::AudioBlock<float> audio, const cell::ParameterBank& bank, const cell::Transport& clock,
                 std::span<const cell::MidiEvent> midi) noexcept override {
        processSidechain(audio, bank, clock, midi, {});
    }
    void processSidechain(cell::AudioBlock<float> audio, const cell::ParameterBank& bank,
                          const cell::Transport& clock, std::span<const cell::MidiEvent> midi,
                          cell::AudioBlock<float> sidechain) noexcept override {
        if (observations < observed.size()) observed[observations] = clock;
        ++observations;
        observedSidechainChannels = sidechain.channelCount;
        for (auto message : midi) {
            midiInRange &= message.frame < audio.frames;
            message.frame += audio.offset;
            if (observedMidiCount < observedMidi.size()) observedMidi[observedMidiCount] = {message, bank.get(17)};
            ++observedMidiCount;
        }
        for (unsigned channel = 0; channel < audio.channelCount; ++channel) {
            const auto value = static_cast<float>(bank.get(channel == 0 ? 17 : 18)) +
                (markChannels ? static_cast<float>(channel + 1) / 64 : 0);
            const auto* key = sidechain.channelCount
                ? sidechain.channel(std::min(channel, sidechain.channelCount - 1)) : nullptr;
            for (unsigned frame = 0; frame < audio.frames; ++frame)
                audio.channel(channel)[frame] += value + (key ? key[frame] : 0);
        }
    }
    bool routesMidi() const noexcept override { return generateMidi; }
    void processMidi(cell::AudioBlock<float> audio, const cell::ParameterBank& bank,
                     const cell::Transport& clock, std::span<const cell::MidiEvent> midi,
                     cell::AudioBlock<float> sidechain, cell::midi::Output& output) noexcept override {
        processSidechain(audio, bank, clock, midi, sidechain);
        for (auto message : midi) {
            const auto kind = message.data[0] & 0xf0;
            if (kind == 0x80 || kind == 0x90) message.data[1] = std::min<unsigned>(127, message.data[1] + 12);
            if (!output.push(message)) return;
        }
        if (audio.offset == edit + 1) output.push({0, {0xc3, 7, 127}, 3});
    }
};

struct HostContext {
    double tempo = 137.5, beat = -0.5, numerator = 7;
    NSInteger denominator = 8;
    AUHostTransportStateFlags flags = AUHostTransportStateMoving | AUHostTransportStateRecording |
        AUHostTransportStateCycling;
    double sample = -960, start = -2, end = 8;
    bool musicalAvailable = true, transportAvailable = true;
    unsigned musicalCalls{}, transportCalls{};

    void connect(AUAudioUnit* unit) {
        auto* host = this;
        unit.musicalContextBlock = ^BOOL(double* bpm, double* num, NSInteger* denom, double* beatPosition,
                                         NSInteger*, double*) {
            if (bpm) *bpm = host->tempo;
            if (num) *num = host->numerator;
            if (denom) *denom = host->denominator;
            if (beatPosition) *beatPosition = host->beat;
            ++host->musicalCalls;
            return host->musicalAvailable;
        };
        unit.transportStateBlock = ^BOOL(AUHostTransportStateFlags* flags, double* position,
                                          double* loopStart, double* loopEnd) {
            if (flags) *flags = host->flags;
            if (position) *position = host->sample;
            if (loopStart) *loopStart = host->start;
            if (loopEnd) *loopEnd = host->end;
            ++host->transportCalls;
            return host->transportAvailable;
        };
    }
};

void require(bool condition, const char* reason) {
    if (!condition) throw std::runtime_error(reason);
}

void allocate(AUAudioUnit* unit) {
    NSError* error = nil;
    if (![unit allocateRenderResourcesAndReturnError:&error])
        throw std::runtime_error(error ? error.localizedDescription.UTF8String : "Render allocation failed");
}

AURenderEvent automationEvent(AUParameterAddress parameter, AUEventSampleTime time,
                              AUAudioFrameCount duration, AUValue value) {
    AURenderEvent event{};
    event.parameter.eventType = duration ? AURenderEventParameterRamp : AURenderEventParameter;
    event.parameter.eventSampleTime = time;
    event.parameter.rampDurationSampleFrames = duration;
    event.parameter.parameterAddress = parameter;
    event.parameter.value = value;
    return event;
}

template <std::size_t Channels = 2>
auto render(AUAudioUnit* unit, const AURenderEvent* event = nullptr, AUEventSampleTime position = renderTime,
            AURenderPullInputBlock input = nil, AUAudioUnitStatus expected = noErr, AURenderBlock hostRender = nil) {
    struct Buffers {
        UInt32 count;
        AudioBuffer channels[Channels];
    };
    static_assert(offsetof(Buffers, channels) == offsetof(AudioBufferList, mBuffers));
    std::array<std::array<float, frames>, Channels> output{};
    Buffers buffers{Channels, {}};
    for (unsigned channel = 0; channel < Channels; ++channel)
        buffers.channels[channel] = {1, sizeof(output[channel]), output[channel].data()};
    AudioTimeStamp time{};
    time.mSampleTime = static_cast<double>(position);
    time.mFlags = kAudioTimeStampSampleTimeValid;
    if (!input) {
        input = ^AUAudioUnitStatus(AudioUnitRenderActionFlags* flags, const AudioTimeStamp*,
                                  AUAudioFrameCount, NSInteger bus, AudioBufferList*) {
            if (bus != 0) return kAudioUnitErr_InvalidElement;
            *flags |= kAudioUnitRenderAction_OutputIsSilence;
            return noErr;
        };
    }
    observations = 0;
    observedSidechainChannels = 0;
    observedMidiCount = 0;
    midiInRange = true;
    AudioUnitRenderActionFlags flags{};
    auto* outputList = reinterpret_cast<AudioBufferList*>(&buffers);
    const auto status = hostRender ? hostRender(&flags, &time, frames, 0, outputList, input)
        : unit.internalRenderBlock(&flags, &time, frames, 0, outputList, event, input);
    require(status == expected && (observations != 0) == (expected == noErr), "Unexpected AUv3 render status");
    return output;
}

void requireFallback(bool offline) {
    const auto& clock = observed[0];
    require(clock.offline == offline && !clock.tempoValid && clock.tempo == 120 &&
        !clock.beatPositionValid && !clock.samplePositionValid && !clock.loopPointsValid &&
        !clock.playing && !clock.recording && !clock.looping &&
        clock.numerator == 4 && clock.denominator == 4, "Incorrect fallback host clock");
}

void verifyRamps(AUAudioUnit* unit) {
    AUParameter* edit = [unit.parameterTree parameterWithAddress:17];
    AUParameter* mode = [unit.parameterTree parameterWithAddress:18];
    require((edit.flags & kAudioUnitParameterFlag_CanRamp) && mode &&
        !(mode.flags & kAudioUnitParameterFlag_CanRamp), "Incorrect AUv3 ramp metadata");
    edit.value = 0;
    auto ramp = automationEvent(17, renderTime + frames - 2, 4, 1);
    auto discreteRamp = automationEvent(18, renderTime + frames - 2, 4, 1);
    ramp.parameter.next = &discreteRamp;
    auto output = render(unit, &ramp);
    require(output[0][frames - 3] == 0 && output[0][frames - 2] == 0 &&
        output[0][frames - 1] == 0.25f, "AUv3 ramp began at the wrong sample");
    const auto point = automationEvent(18, renderTime + frames + 3, 0, 1);
    output = render(unit, &point, renderTime + frames);
    require(output[0][0] == 0.5f && output[0][1] == 0.75f && output[0][2] == 1 &&
        output[0].back() == 1, "AUv3 ramp state did not cross the render boundary");
    require(output[1][1] == 0 && output[1][2] == 0 && output[1][3] == 1,
        "A non-rampable parameter did not stay point-only");
}

void verifySidechain(AUAudioUnit* unit) {
    AUAudioUnitBus* bus = unit.inputBusses[1];
    NSError* error = nil;
    bus.enabled = YES;
    const auto mono = [[AVAudioFormat alloc] initStandardFormatWithSampleRate:sampleRate channels:1];
    const auto wrongRate = [[AVAudioFormat alloc] initStandardFormatWithSampleRate:44100 channels:1];
    require([bus setFormat:wrongRate error:&error], "Could not configure sidechain negotiation test");
    require(![unit allocateRenderResourcesAndReturnError:&error] &&
        error.code == kAudioUnitErr_FormatNotSupported, "Mismatched sidechain sample rate was accepted");
    require([bus setFormat:mono error:&error], "Mono sidechain format rejected");
    allocate(unit);
    [unit.parameterTree parameterWithAddress:17].value = 0;
    [unit.parameterTree parameterWithAddress:18].value = 0;

    struct KeySignal {
        std::array<std::array<float, frames>, 2> samples{};
        UInt32 channels = 1;
        bool silent{};
        AUAudioUnitStatus status{};
    } key;
    for (unsigned frame = 0; frame < frames; ++frame) {
        key.samples[0][frame] = static_cast<float>(frame) / 256;
        key.samples[1][frame] = -static_cast<float>(frame) / 256;
    }
    auto* source = &key;
    AURenderPullInputBlock pull = ^AUAudioUnitStatus(AudioUnitRenderActionFlags* flags, const AudioTimeStamp*,
                                                    AUAudioFrameCount count, NSInteger inputBus, AudioBufferList* data) {
        if (count != frames || inputBus < 0 || inputBus > 1) return kAudioUnitErr_InvalidElement;
        if (data->mNumberBuffers != (inputBus == 0 ? 2u : source->channels))
            return kAudioUnitErr_FormatNotSupported;
        if (inputBus == 1 && source->status != noErr) return source->status;
        if (inputBus == 1 && source->silent) *flags |= kAudioUnitRenderAction_OutputIsSilence;
        for (UInt32 channel = 0; channel < data->mNumberBuffers; ++channel) {
            auto& buffer = data->mBuffers[channel];
            if (!buffer.mData || buffer.mDataByteSize != count * sizeof(float))
                return kAudioUnitErr_InvalidPropertyValue;
            if (inputBus == 0)
                std::fill_n(static_cast<float*>(buffer.mData), count, channel == 0 ? 0.25f : -0.5f);
            else
                buffer.mData = source->silent ? nullptr : source->samples[channel].data();
        }
        return noErr;
    };
    const auto ramp = automationEvent(17, renderTime + edit, 4, 1);
    auto output = render(unit, &ramp, renderTime, pull);
    require(observedSidechainChannels == 1, "Mono sidechain lost its independent channel count");
    for (unsigned frame = 0; frame < frames; ++frame) {
        const auto value = frame > edit ? static_cast<float>(std::min(frame - edit, 4u)) / 4 : 0.f;
        require(output[0][frame] == 0.25f + (value + key.samples[0][frame]) &&
            output[1][frame] == -0.5f + key.samples[0][frame], "Sidechain shifted during an automation ramp");
    }
    [unit deallocateRenderResources];

    const auto stereo = [[AVAudioFormat alloc] initStandardFormatWithSampleRate:sampleRate channels:2];
    require([bus setFormat:stereo error:&error], "Stereo sidechain format rejected");
    key.channels = 2;
    allocate(unit);
    [unit.parameterTree parameterWithAddress:17].value = 0;
    output = render(unit, nullptr, renderTime, pull);
    require(observedSidechainChannels == 2 && output[0].back() == 0.25f + key.samples[0].back() &&
        output[1].back() == -0.5f + key.samples[1].back(), "Stereo sidechain channels were mixed up");

    key.silent = true;
    output = render(unit, nullptr, renderTime, pull);
    require(observedSidechainChannels == 2 && output[0].back() == 0.25f && output[1].back() == -0.5f,
        "Connected silent sidechain reused earlier samples");
    key.silent = false;
    key.status = kAudioUnitErr_NoConnection;
    output = render(unit, nullptr, renderTime, pull);
    require(observedSidechainChannels == 0 && output[0].back() == 0.25f && output[1].back() == -0.5f,
        "Disconnected sidechain affected the main input");
    key.status = kAudioUnitErr_CannotDoInCurrentContext;
    render(unit, nullptr, renderTime, pull, key.status);

    key.status = noErr;
    unit.shouldBypassEffect = YES;
    output = render(unit, nullptr, renderTime, pull);
    require(observedSidechainChannels == 2 && output[0].back() == 0.25f && output[1].back() == -0.5f,
        "Sidechain leaked into dry bypass");
    [unit deallocateRenderResources];
}

AURenderEvent midiEvent(AUEventSampleTime time, std::array<std::uint8_t, 3> bytes,
                        std::uint16_t length = 3, std::uint8_t cable = 0) {
    AURenderEvent event{};
    event.MIDI.eventType = AURenderEventMIDI;
    event.MIDI.eventSampleTime = time;
    event.MIDI.cable = cable;
    event.MIDI.length = length;
    std::copy(bytes.begin(), bytes.end(), event.MIDI.data);
    return event;
}

void requireMidi(std::size_t index, std::uint32_t frame, std::uint8_t size, std::array<std::uint8_t, 3> bytes) {
    require(midiInRange && index < observedMidiCount, "Missing or out-of-range MIDI event");
    const auto& event = observedMidi[index].event;
    require(event.frame == frame && event.size == size && event.data == bytes, "Incorrect MIDI timing or payload");
}

void verifyMidi(AUAudioUnit* unit) {
    const auto hostRender = unit.renderBlock;
    const auto schedule = unit.scheduleMIDIEventBlock;
    const auto scheduleList = unit.scheduleMIDIEventListBlock;
    require(unit.isMusicDeviceOrEffect && unit.virtualMIDICableCount == 1 && hostRender && schedule && scheduleList,
        "MIDI effect did not publish its scheduling API");
    allocate(unit);
    [unit.parameterTree parameterWithAddress:17].value = 0;
    std::array events{
        midiEvent(AUEventSampleTimeImmediate, {0x91, 60, 100}),
        automationEvent(17, renderTime + edit, 4, 1),
        midiEvent(renderTime + edit, {0xb1, 74, 45}),
        midiEvent(renderTime + edit + 1, {0x91, 61, 100}, 3, 1),
        midiEvent(renderTime + edit + 2, {0xc1, 10, 127}, 2),
        midiEvent(renderTime + edit + 3, {0x91, 130, 100}),
        midiEvent(renderTime + edit + 4, {0xc1, 10, 0}),
        midiEvent(renderTime + edit + 5, {0xf8, 0, 0}, 1),
        midiEvent(renderTime + frames - 1, {0x81, 60, 0}),
        midiEvent(renderTime + frames, {0x91, 62, 100}),
    };
    for (std::size_t i = 1; i < events.size(); ++i) events[i - 1].head.next = &events[i];
    render(unit, events.data());
    require(observedMidiCount == 4, "Unsupported or out-of-block MIDI was delivered");
    requireMidi(0, 0, 3, {0x91, 60, 100});
    requireMidi(1, edit, 3, {0xb1, 74, 45});
    requireMidi(2, edit + 2, 2, {0xc1, 10, 0});
    requireMidi(3, frames - 1, 3, {0x81, 60, 0});
    require(observedMidi[2].parameter == .5, "MIDI did not coincide with the parameter ramp value");

    constexpr AUEventSampleTime next = renderTime + frames;
    constexpr std::array<std::uint8_t, 3> note{0x92, 64, 90};
    schedule(next + 4, 0, note.size(), note.data());
    MIDIEventList list{};
    auto* packet = MIDIEventListInit(&list, kMIDIProtocol_1_0);
    constexpr auto cc = MIDI1UPControlChange(0, 2, 7, 100);
    require(MIDIEventListAdd(&list, sizeof(list), packet, 0, 1, &cc) != nullptr,
        "Could not construct the scheduled MIDI event list");
    require(scheduleList(next + 8, 0, &list) == noErr, "AUv3 rejected a MIDI 1.0 event list");
    render(unit, nullptr, next, nil, noErr, hostRender);
    require(observedMidiCount == 2, "Public scheduling did not deliver both MIDI messages");
    requireMidi(0, 4, 3, note);
    requireMidi(1, 8, 3, {0xb2, 7, 100});

    const auto resetCount = resets;
    std::vector<AURenderEvent> dense(cell::midi::eventCapacity + 1, midiEvent(next, {0x82, 64, 0}));
    for (std::size_t i = 1; i < dense.size(); ++i) dense[i - 1].head.next = &dense[i];
    render(unit, dense.data(), next, nil, kAudioUnitErr_CannotDoInCurrentContext);
    require(observedMidiCount == 0 && resets == resetCount + 1, "AUv3 did not release state on MIDI overflow");
    [unit deallocateRenderResources];
}

void verifyMidiOutput(AUAudioUnit* unit) {
    require([unit.MIDIOutputNames isEqualToArray:@[@"MIDI Output"]], "AUv3 MIDI output metadata is missing");
    struct Capture {
        struct Event {
            AUEventSampleTime time{};
            std::array<std::uint8_t, 3> bytes{};
            NSInteger length{};
        };
        std::array<Event, 64> events{};
        std::size_t count{}, limit = events.size();
    } capture;
    auto* received = &capture;
    unit.MIDIOutputEventBlock = ^OSStatus(AUEventSampleTime time, std::uint8_t cable, NSInteger length,
                                          const std::uint8_t* bytes) {
        if (received->count == received->limit || cable != 0 || (length != 2 && length != 3))
            return kAudioUnitErr_CannotDoInCurrentContext;
        auto& event = received->events[received->count++];
        event = {time, {}, length};
        std::copy_n(bytes, length, event.bytes.begin());
        return noErr;
    };
    allocate(unit);
    generateMidi = true;
    [unit.parameterTree parameterWithAddress:17].value = 0;
    std::array events{
        midiEvent(renderTime, {0x91, 60, 100}),
        automationEvent(17, renderTime + edit, 4, 1),
        midiEvent(renderTime + edit, {0xb1, 74, 45}),
        midiEvent(renderTime + edit + 9, {0x81, 60, 0}),
    };
    for (std::size_t i = 1; i < events.size(); ++i) events[i - 1].head.next = &events[i];
    render(unit, events.data());
    require(capture.count == 4 && capture.events[0].time == renderTime &&
        capture.events[0].bytes == std::array<std::uint8_t, 3>{0x91, 72, 100} &&
        capture.events[1].time == renderTime + edit &&
        capture.events[2].time == renderTime + edit + 1 && capture.events[2].length == 2 &&
        capture.events[2].bytes == std::array<std::uint8_t, 3>{0xc3, 7, 0} &&
        capture.events[3].time == renderTime + edit + 9 && capture.events[3].bytes[1] == 72,
        "AUv3 output lost transformed or generated MIDI timing");

    constexpr AUEventSampleTime next = renderTime + frames;
    auto on = midiEvent(next, {0x91, 60, 100});
    auto off = midiEvent(next + 4, {0x81, 60, 0});
    on.head.next = &off;
    capture.count = 0;
    capture.limit = 1;
    render(unit, &on, next);
    require(capture.count == 1, "MIDI backpressure did not stop the batch");

    capture.count = 0;
    capture.limit = 4;
    render(unit, &on, next);
    require(capture.count == 4 && capture.events[0].time == next &&
        capture.events[0].bytes == std::array<std::uint8_t, 3>{0xb0, 64, 0} &&
        capture.events[3].bytes == std::array<std::uint8_t, 3>{0xb1, 64, 0},
        "Partial MIDI recovery did not release channels before new notes");

    on.head.next = nullptr;
    capture.count = 0;
    capture.limit = capture.events.size();
    render(unit, &on, next);
    require(capture.count == 45 && capture.events[0].bytes == std::array<std::uint8_t, 3>{0xb1, 120, 0} &&
        capture.events[43].bytes == std::array<std::uint8_t, 3>{0xbf, 123, 0} && capture.events[44].bytes[1] == 72,
        "MIDI recovery did not resume at the rejected controller");

    unit.shouldBypassEffect = YES;
    [unit.parameterTree parameterWithAddress:17].value = 0;
    capture.count = 0;
    render(unit, events.data());
    require(capture.count == 51 && capture.events[48].bytes[1] == 60 && capture.events[50].bytes[1] == 60,
        "MIDI bypass changed incoming messages or leaked generated messages");

    capture.count = 0;
    [unit reset];
    render(unit, nullptr, next);
    require(capture.count == 48 && capture.events[47].bytes == std::array<std::uint8_t, 3>{0xbf, 123, 0},
        "Reset did not release notes at the next render");
    [unit deallocateRenderResources];
    unit.MIDIOutputEventBlock = nil;
    unit.shouldBypassEffect = NO;
    generateMidi = false;

    struct EventListCapture {
        AUEventSampleTime time{};
        UInt32 word{};
        unsigned count{};
    } lists;
    auto* list = &lists;
    unit.hostMIDIProtocol = kMIDIProtocol_1_0;
    unit.MIDIOutputEventListBlock = ^OSStatus(AUEventSampleTime time, std::uint8_t cable, const MIDIEventList* events) {
        if (cable != 0 || events->protocol != kMIDIProtocol_1_0 || events->numPackets != 1 ||
            events->packet[0].wordCount != 1) return kAudioUnitErr_InvalidPropertyValue;
        list->time = time;
        list->word = events->packet[0].words[0];
        ++list->count;
        return noErr;
    };
    allocate(unit);
    const auto input = midiEvent(next + 4, {0x91, 60, 100});
    render(unit, &input, next);
    require(lists.count == 1 && lists.time == next + 4 && lists.word == MIDI1UPNoteOn(0, 1, 60, 100),
        "Core Audio did not bridge the AUv3 MIDI output to the host event-list callback");
    [unit deallocateRenderResources];
    unit.MIDIOutputEventListBlock = nil;
}

AVAudioFormat* formatFor(AudioChannelLayoutTag tag) {
    AVAudioChannelLayout* layout = [AVAudioChannelLayout layoutWithLayoutTag:tag];
    return [[AVAudioFormat alloc] initStandardFormatWithSampleRate:sampleRate channelLayout:layout];
}

template <std::size_t Channels>
void verifyChannelOrder(AUAudioUnit* unit, AVAudioFormat* inputFormat, AVAudioFormat* outputFormat,
                        const std::array<unsigned, Channels>& inputOrder,
                        const std::array<unsigned, Channels>& outputOrder) {
    NSError* error = nil;
    require([unit.inputBusses[0] setFormat:inputFormat error:&error] &&
        [unit.outputBusses[0] setFormat:outputFormat error:&error],
        error ? error.localizedDescription.UTF8String : "Could not negotiate a named layout");
    allocate(unit);
    [unit.parameterTree parameterWithAddress:17].value = 0;
    [unit.parameterTree parameterWithAddress:18].value = 0;
    auto pull = ^AUAudioUnitStatus(AudioUnitRenderActionFlags*, const AudioTimeStamp*, AUAudioFrameCount count,
                                   NSInteger bus, AudioBufferList* data) {
        if (bus != 0 || data->mNumberBuffers != Channels) return kAudioUnitErr_FormatNotSupported;
        for (unsigned channel = 0; channel < Channels; ++channel)
            std::fill_n(static_cast<float*>(data->mBuffers[channel].mData), count, static_cast<float>(channel) / 32);
        return noErr;
    };
    markChannels = true;
    unit.shouldBypassEffect = NO;
    const auto wet = render<Channels>(unit, nullptr, renderTime, pull);
    unit.shouldBypassEffect = YES;
    const auto dry = render<Channels>(unit, nullptr, renderTime + frames, pull);
    markChannels = false;
    [unit deallocateRenderResources];
    for (unsigned channel = 0; channel < Channels; ++channel) {
        const auto original = static_cast<float>(inputOrder[channel]) / 32;
        const auto processed = original + static_cast<float>(channel + 1) / 64;
        const auto output = outputOrder[channel];
        require(std::all_of(wet[output].begin(), wet[output].end(), [&](float sample) { return sample == processed; }),
            "The processor received or wrote the wrong speaker");
        require(std::all_of(dry[output].begin(), dry[output].end(), [&](float sample) { return sample == original; }),
            "Bypass routed audio to the wrong speaker");
    }
}

void verifyNamedLayouts(AUAudioUnit* unit) {
    unit.inputBusses[1].enabled = NO;
    require([unit.inputBusses[0].supportedChannelLayoutTags containsObject:@(kAudioChannelLayoutTag_Atmos_9_1_6)] &&
        [unit.outputBusses[0].supportedChannelCounts containsObject:@16],
        "AUv3 did not publish the declared immersive layouts");
    NSError* error = nil;
    require(![unit.inputBusses[0] setFormat:formatFor(kAudioChannelLayoutTag_Atmos_5_1_4) error:&error],
        "AUv3 accepted 5.1.4 as 7.1.2 because their widths match");
    auto* five = formatFor(kAudioChannelLayoutTag_MPEG_5_1_D);
    auto* seven = formatFor(kAudioChannelLayoutTag_Atmos_7_1_2);
    require([unit.inputBusses[0] setFormat:five error:&error] &&
        [unit.outputBusses[0] setFormat:seven error:&error], "Could not set up mismatched layout test");
    require(![unit allocateRenderResourcesAndReturnError:&error] && error.code == kAudioUnitErr_FormatNotSupported,
        "Mismatched input and output layouts were accepted");

    const std::array<unsigned, 1> mono{0};
    verifyChannelOrder(unit, formatFor(kAudioChannelLayoutTag_Mono), formatFor(kAudioChannelLayoutTag_Mono), mono, mono);
    verifyChannelOrder(unit, five, formatFor(kAudioChannelLayoutTag_MPEG_5_1_B),
        std::array<unsigned, 6>{1, 2, 0, 5, 3, 4}, std::array<unsigned, 6>{0, 1, 4, 5, 2, 3});
    const std::array<unsigned, 10> order712{0, 1, 2, 3, 6, 7, 4, 5, 8, 9};
    verifyChannelOrder(unit, seven, seven, order712, order712);
    verifyChannelOrder(unit, formatFor(kAudioChannelLayoutTag_Logic_Atmos_7_1_4_B),
        formatFor(kAudioChannelLayoutTag_Atmos_7_1_4),
        std::array<unsigned, 12>{0, 1, 6, 7, 2, 3, 4, 5, 8, 9, 10, 11},
        std::array<unsigned, 12>{0, 1, 2, 3, 6, 7, 4, 5, 8, 9, 10, 11});
    auto* sixteen = formatFor(kAudioChannelLayoutTag_Atmos_9_1_6);
    const std::array<unsigned, 16> order916{0, 1, 2, 3, 6, 7, 4, 5, 10, 11, 14, 15, 12, 13, 8, 9};
    verifyChannelOrder(unit, sixteen, sixteen, order916, order916);
}

void verifyUmp(AUAudioUnit* unit) {
    constexpr std::array<UInt32, 6> sequence{
        0x40913c00u, 0x92a00000u, // note-on with 16-bit velocity
        0x40113c4au, 0xb1234567u, // assignable per-note controller
        0x40613c00u, 0x81234567u, // per-note pitch bend
    };
    struct Capture {
        std::array<UInt32, 6> words{};
        unsigned count{};
    } captured;
    auto* out = &captured;
    require(unit.AudioUnitMIDIProtocol == kMIDIProtocol_2_0, "AUv3 did not advertise the MIDI 2.0 protocol");
    unit.hostMIDIProtocol = kMIDIProtocol_2_0;
    unit.MIDIOutputEventListBlock = ^OSStatus(AUEventSampleTime time, std::uint8_t cable, const MIDIEventList* events) {
        if (time != renderTime + edit || cable != 0 || events->protocol != kMIDIProtocol_2_0 ||
            events->numPackets != 1 || events->packet[0].wordCount != 2 || out->count >= 3)
            return kAudioUnitErr_InvalidPropertyValue;
        std::copy_n(events->packet[0].words, 2, out->words.begin() + 2 * out->count++);
        return noErr;
    };
    allocate(unit);
    MIDIEventList list{};
    auto* packet = MIDIEventListInit(&list, kMIDIProtocol_2_0);
    require(MIDIEventListAdd(&list, sizeof(list), packet, 0, sequence.size(), sequence.data()) != nullptr,
        "Could not build the AUv3 expression event list");
    const auto schedule = unit.scheduleMIDIEventListBlock;
    require(schedule && schedule(renderTime + edit, 0, &list) == noErr, "AU host could not schedule UMP");
    render(unit, nullptr, renderTime, nil, noErr, unit.renderBlock);
    require(observedMidiCount == 3 && midiInRange, "Processor did not receive complete AUv3 UMP messages");
    for (unsigned i = 0; i < 3; ++i) {
        const auto& event = observedMidi[i].event;
        require(event.frame == edit && event.umpCount == 2 && event.ump[0] == sequence[2 * i] &&
            event.ump[1] == sequence[2 * i + 1], "AUv3 changed UMP data or sample timing");
    }
    require(captured.count == 3 && captured.words == sequence, "AUv3 output changed UMP order or precision");
    [unit deallocateRenderResources];
}

void run() {
    HostContext host;
    [AUAudioUnit registerSubclass:CellAUv3Effect.class asComponentDescription:description
        name:@"Cell: Transport Contract" version:1];
    NSError* error = nil;
    AUAudioUnit* unit = [[CellAUv3Effect alloc] initWithComponentDescription:description options:0 error:&error];
    if (!unit) throw std::runtime_error(error ? error.localizedDescription.UTF8String : "Could not open AUv3");
    require(unit.inputBusses.count == 2 && unit.outputBusses.count == 1 &&
        [unit.inputBusses[1].name isEqualToString:@"Sidechain"] && !unit.inputBusses[1].isEnabled,
        "Optional sidechain bus was not published correctly");
    unit.maximumFramesToRender = frames;
    if (verifyUmpOnly) {
        verifyUmp(unit);
        return;
    }
    allocate(unit);
    render(unit);
    requireFallback(false);
    [unit deallocateRenderResources];

    host.connect(unit);
    unit.renderingOffline = YES;
    allocate(unit);
    const auto musicalCalls = host.musicalCalls, transportCalls = host.transportCalls;
    const auto point = automationEvent(17, renderTime + edit, 0, 0.5f);
    render(unit, &point);
    require(observations == 2, "AUv3 point automation did not split the render");
    require(host.musicalCalls == musicalCalls + 1 && host.transportCalls == transportCalls + 1,
        "Expected one host clock read per render quantum");
    const auto& clock = observed[0];
    require(clock.offline && clock.playing && clock.recording && clock.looping && clock.tempoValid &&
        clock.tempo == 137.5 && clock.numerator == 7 && clock.denominator == 8 &&
        clock.beatPositionValid && clock.beatPosition == -0.5 &&
        clock.samplePositionValid && clock.samplePosition == -960 &&
        clock.loopPointsValid && clock.loopStartBeat == -2 && clock.loopEndBeat == 8,
        "Complete host clock did not reach the processor");
    const auto& split = observed[1];
    require(split.samplePositionValid && split.samplePosition == clock.samplePosition + edit &&
        split.tempoValid && split.beatPositionValid &&
        std::abs(split.beatPosition - (-0.5 + edit / sampleRate * 137.5 / 60)) < 1e-12,
        "Host clock did not advance to the split offset");

    host.tempo = -1;
    host.beat = 0;
    host.numerator = 7.5;
    host.sample = 0x1p63;
    host.start = 9;
    render(unit);
    require(!clock.tempoValid && clock.tempo == 120 && clock.beatPositionValid && clock.beatPosition == 0 &&
        !clock.samplePositionValid && clock.looping && !clock.loopPointsValid &&
        clock.numerator == 4 && clock.denominator == 4, "Malformed values polluted the clock");

    host.musicalAvailable = false;
    host.flags = AUHostTransportStateRecording;
    host.sample = 0;
    host.start = -2;
    render(unit);
    require(!clock.tempoValid && !clock.beatPositionValid && clock.samplePositionValid &&
        clock.samplePosition == 0 && clock.recording && !clock.playing && !clock.loopPointsValid,
        "Musical context availability affected transport state");

    host.transportAvailable = false;
    render(unit);
    requireFallback(true);
    [unit deallocateRenderResources];

    unit.musicalContextBlock = nil;
    unit.transportStateBlock = nil;
    unit.renderingOffline = NO;
    allocate(unit);
    render(unit);
    requireFallback(false);
    verifyRamps(unit);
    [unit deallocateRenderResources];
    verifyMidiOutput(unit);
    verifyMidi(unit);
    verifySidechain(unit);
    verifyNamedLayouts(unit);
}
} // namespace

namespace cell::plugin {
const PluginDescriptor& descriptor() {
    static const PluginDescriptor value{.id = "com.example.cell.transport", .name = "Cell Transport Contract",
        .vendor = "Cell", .version = "0.1.0", .channels = 2, .parameters = fields,
        .supportsSidechain = true, .acceptsMidi = true, .mainBusLayouts = mainLayouts, .producesMidi = true,
        .supportsUmp = verifyUmpOnly};
    return value;
}
std::unique_ptr<Processor> createProcessor() { return std::make_unique<Probe>(); }
} // namespace cell::plugin

int main(int argc, char** argv) {
    @autoreleasepool {
        try {
            verifyUmpOnly = argc == 2 && std::string_view(argv[1]) == "--ump";
            run();
            std::cout << (verifyUmpOnly ? "AUv3 MIDI 2.0 round trip passed.\n" :
                "AUv3 channel layouts, transport, automation, sidechain and MIDI checks passed.\n");
            return 0;
        } catch (const std::exception& error) {
            std::cerr << error.what() << '\n';
            return 1;
        }
    }
}
