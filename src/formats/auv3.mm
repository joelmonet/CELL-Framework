// Original Cell Framework implementation; see LICENSE.
#import <AudioToolbox/AudioToolbox.h>
#import <AudioToolbox/AUAudioUnitImplementation.h>
#import <AVFoundation/AVFoundation.h>

#include <cell/callback_gate.hpp>
#include <cell/midi_delivery.hpp>
#if defined(CELL_AUV3_EDITOR_METERS)
#include <cell/web_meter.hpp>
#endif
#include "auv3_automation.hpp"
#include "auv3_layouts.hpp"

namespace {
namespace auv3 = cell::auv3;
constexpr std::size_t maxScheduled = 4096;
constexpr std::size_t maxStateBytes = 128 * 1024 * 1024;
constexpr double noPending = std::numeric_limits<double>::quiet_NaN();

void reportError(NSError** error, OSStatus status) {
    if (error) *error = [NSError errorWithDomain:NSOSStatusErrorDomain code:status userInfo:nil];
}

bool supportsPCM(AVAudioFormat* format) {
    return format.commonFormat == AVAudioPCMFormatFloat32 && !format.isInterleaved &&
        std::isfinite(format.sampleRate) && format.sampleRate >= 1000 && format.sampleRate <= 768000;
}

bool supportsMonoStereoPCM(AVAudioFormat* format) {
    return supportsPCM(format) && (format.channelCount == 1 || format.channelCount == 2);
}

struct LayoutTag {
    cell::AudioLayout layout;
    AudioChannelLayoutTag tag;
};
using cell::AudioLayout;
constexpr LayoutTag layoutTags[] = {
    {AudioLayout::mono, kAudioChannelLayoutTag_Mono},
    {AudioLayout::stereo, kAudioChannelLayoutTag_Stereo},
    {AudioLayout::surround51, kAudioChannelLayoutTag_MPEG_5_1_A},
    {AudioLayout::surround51, kAudioChannelLayoutTag_MPEG_5_1_B},
    {AudioLayout::surround51, kAudioChannelLayoutTag_MPEG_5_1_C},
    {AudioLayout::surround51, kAudioChannelLayoutTag_MPEG_5_1_D},
    {AudioLayout::surround51, kAudioChannelLayoutTag_WAVE_5_1_B},
    {AudioLayout::surround712, kAudioChannelLayoutTag_Atmos_7_1_2},
    {AudioLayout::surround714, kAudioChannelLayoutTag_Atmos_7_1_4},
    {AudioLayout::surround714, kAudioChannelLayoutTag_Logic_Atmos_7_1_4_B},
    {AudioLayout::surround916, kAudioChannelLayoutTag_Atmos_9_1_6},
};

struct LayoutStorage {
    AudioChannelLayoutTag tag{};
    AudioChannelBitmap bitmap{};
    UInt32 count{};
    AudioChannelDescription descriptions[auv3::maxChannels]{};
};
static_assert(offsetof(LayoutStorage, descriptions) == offsetof(AudioChannelLayout, mChannelDescriptions));

using auv3::Speaker;
Speaker speakerFor(AudioChannelLabel label, unsigned channels, bool hasRear) noexcept {
    switch (label) {
        case kAudioChannelLabel_Mono: return Speaker::mono;
        case kAudioChannelLabel_Left: return Speaker::left;
        case kAudioChannelLabel_Right: return Speaker::right;
        case kAudioChannelLabel_Center: return channels == 1 ? Speaker::mono : Speaker::center;
        case kAudioChannelLabel_LFEScreen: return Speaker::lfe;
        case kAudioChannelLabel_LeftSurround: return hasRear && channels > 6 ? Speaker::sideLeft : Speaker::rearLeft;
        case kAudioChannelLabel_RightSurround: return hasRear && channels > 6 ? Speaker::sideRight : Speaker::rearRight;
        case kAudioChannelLabel_LeftSideSurround:
        case kAudioChannelLabel_LeftSurroundDirect: return channels == 6 ? Speaker::rearLeft : Speaker::sideLeft;
        case kAudioChannelLabel_RightSideSurround:
        case kAudioChannelLabel_RightSurroundDirect: return channels == 6 ? Speaker::rearRight : Speaker::sideRight;
        case kAudioChannelLabel_RearSurroundLeft:
        case kAudioChannelLabel_LeftBackSurround: return Speaker::rearLeft;
        case kAudioChannelLabel_RearSurroundRight:
        case kAudioChannelLabel_RightBackSurround: return Speaker::rearRight;
        case kAudioChannelLabel_LeftTopFront: return Speaker::topFrontLeft;
        case kAudioChannelLabel_RightTopFront: return Speaker::topFrontRight;
        case kAudioChannelLabel_TopBackLeft:
        case kAudioChannelLabel_LeftTopRear: return Speaker::topRearLeft;
        case kAudioChannelLabel_TopBackRight:
        case kAudioChannelLabel_RightTopRear: return Speaker::topRearRight;
        case kAudioChannelLabel_LeftTopMiddle: return Speaker::topMiddleLeft;
        case kAudioChannelLabel_RightTopMiddle: return Speaker::topMiddleRight;
        case kAudioChannelLabel_LeftWide: return Speaker::wideLeft;
        case kAudioChannelLabel_RightWide: return Speaker::wideRight;
        default: return Speaker::unknown;
    }
}

std::optional<auv3::ChannelOrder> mainOrder(AVAudioFormat* format, const cell::PluginDescriptor& descriptor,
                                          bool outputOnly = false) {
    if (!supportsPCM(format)) return {};
    const auto channels = format.channelCount;
    const auto layout = std::find_if(auv3::mainLayouts.begin(), auv3::mainLayouts.end(), [&](AudioLayout candidate) {
        return cell::channelCount(candidate) == channels && auv3::supportsMainLayout(descriptor, candidate, outputOnly);
    });
    if (layout == auv3::mainLayouts.end()) return {};
    if (descriptor.mainBusLayouts.empty()) return auv3::identityOrder;
    const auto* native = format.channelLayout.layout;
    if (!native || (native->mChannelLayoutTag & 0xffff0000u) == kAudioChannelLayoutTag_Unknown)
        return channels <= 2 ? std::optional{auv3::identityOrder} : std::nullopt;
    if ((*layout == AudioLayout::mono && native->mChannelLayoutTag == kAudioChannelLayoutTag_Mono) ||
        (*layout == AudioLayout::stereo && native->mChannelLayoutTag == kAudioChannelLayoutTag_Stereo))
        return auv3::identityOrder;

    LayoutStorage decoded;
    const AudioChannelDescription* descriptions = native->mChannelDescriptions;
    if (native->mChannelLayoutTag == kAudioChannelLayoutTag_UseChannelDescriptions) {
        if (native->mNumberChannelDescriptions != channels) return {};
    } else {
        const bool bitmap = native->mChannelLayoutTag == kAudioChannelLayoutTag_UseChannelBitmap;
        const UInt32 specifier = bitmap ? native->mChannelBitmap : native->mChannelLayoutTag;
        UInt32 bytes = sizeof(decoded);
        const auto property = bitmap ? kAudioFormatProperty_ChannelLayoutForBitmap : kAudioFormatProperty_ChannelLayoutForTag;
        if (AudioFormatGetProperty(property, sizeof(specifier), &specifier, &bytes, &decoded) != noErr ||
            bytes < offsetof(LayoutStorage, descriptions) + channels * sizeof(AudioChannelDescription) ||
            decoded.count != channels) return {};
        descriptions = decoded.descriptions;
    }
    const bool hasRear = std::any_of(descriptions, descriptions + channels, [](auto description) {
        const auto label = description.mChannelLabel;
        return label == kAudioChannelLabel_RearSurroundLeft || label == kAudioChannelLabel_RearSurroundRight ||
            label == kAudioChannelLabel_LeftBackSurround || label == kAudioChannelLabel_RightBackSurround;
    });
    std::array<Speaker, auv3::maxChannels> host{};
    for (unsigned channel = 0; channel < channels; ++channel)
        host[channel] = speakerFor(descriptions[channel].mChannelLabel, channels, hasRear);
    return auv3::channelOrder(*layout, std::span(host).first(channels));
}

std::optional<AUEventSampleTime> samplePosition(double sample, UInt32 frames) noexcept {
    if (frames == 0 || !std::isfinite(sample) || sample < -0x1p63 || sample >= 0x1p63) return {};
    const auto position = static_cast<AUEventSampleTime>(sample);
    if (position > std::numeric_limits<AUEventSampleTime>::max() - (frames - 1)) return {};
    return position;
}

std::uint8_t midiMessageSize(std::uint8_t status) noexcept {
    const auto kind = status & 0xf0;
    return kind == 0xc0 || kind == 0xd0 ? 2 : 3;
}

class HostMidiOutput final : public cell::midi::Output {
public:
    HostMidiOutput(__unsafe_unretained AUMIDIOutputEventBlock block, AUEventSampleTime start) noexcept
        : block_(block), start_(start) {}

    bool push(cell::MidiEvent event) noexcept override {
        if (sent_ == cell::midi::eventCapacity) return false;
        const auto length = midiMessageSize(event.data[0]);
        if (block_(start_ + event.frame, 0, length, event.data.data()) != noErr) return false;
        ++sent_;
        return true;
    }

private:
    __unsafe_unretained AUMIDIOutputEventBlock block_;
    AUEventSampleTime start_;
    std::size_t sent_{};
};

class DiscardMidiOutput final : public cell::midi::Output {
public:
    bool push(cell::MidiEvent) noexcept override { return true; }
};

AudioUnitParameterUnit unitFor(const cell::Parameter& parameter) {
    if (parameter.unit == "dB") return kAudioUnitParameterUnit_Decibels;
    if (parameter.unit == "Hz") return kAudioUnitParameterUnit_Hertz;
    if (parameter.unit == "%") return kAudioUnitParameterUnit_Percent;
    return kAudioUnitParameterUnit_Generic;
}

struct PlanarBuffers {
    UInt32 count{};
    ::AudioBuffer buffers[auv3::maxChannels]{};
    AudioBufferList* list() noexcept { return reinterpret_cast<AudioBufferList*>(this); }
};
static_assert(offsetof(PlanarBuffers, buffers) == offsetof(AudioBufferList, mBuffers));
static_assert(std::atomic<double>::is_always_lock_free);

class RenderState {
public:
    explicit RenderState(bool instrument)
        : engine(cell::plugin::descriptor(), cell::plugin::createProcessor()),
          hasAudioInput(engine.descriptor().channels != 0 && !instrument),
          pending_(std::make_unique<std::atomic<double>[]>(fields().size())),
          automation_(fields().size(), fields().size() + maxScheduled,
                      engine.descriptor().acceptsMidi ? maxScheduled : 0) {
        const auto& descriptor = engine.descriptor();
        if (instrument && (!descriptor.channels || !descriptor.acceptsMidi || descriptor.supportsSidechain))
            throw std::invalid_argument("Cell AUv3 instruments require MIDI input and audio output with no sidechain");
        if (descriptor.channels && !auv3::defaultMainLayout(descriptor, instrument))
            throw std::invalid_argument("Cell AUv3 requires a supported main bus layout");
        for (std::size_t i = 0; i < fields().size(); ++i) pending_[i].store(noPending);
        events_.reserve(fields().size() + maxScheduled);
        if (descriptor.acceptsMidi) midi_.reserve(maxScheduled);
    }

    std::span<const cell::Parameter> fields() const noexcept { return engine.parameters().descriptors(); }

    double value(AUParameterAddress address) const noexcept {
        const auto index = parameterIndex(address);
        if (index == fields().size()) return 0;
        const auto pending = pending_[index].load(std::memory_order_acquire);
        return std::isfinite(pending) ? pending : engine.parameters().get(fields()[index].id);
    }

    void put(AUParameterAddress address, AUValue value) noexcept {
        const auto index = parameterIndex(address);
        if (index < fields().size() && std::isfinite(value))
            pending_[index].store(fields()[index].clamp(value), std::memory_order_release);
    }

    void prepare(double rate, AUAudioFrameCount maximum, UInt32 channels, UInt32 sidechainChannels,
                 auv3::ChannelOrder inputOrder, auv3::ChannelOrder outputOrder) {
        const bool midiOnly = engine.descriptor().channels == 0;
        const UInt32 processorChannels = midiOnly ? 0 : channels;
        cell::AudioBuffer<float> pull(hasAudioInput ? channels : 0, maximum), wet(channels, maximum);
        cell::AudioBuffer<float> sidechainPull, sidechain;
        if (sidechainChannels) {
            sidechainPull = cell::AudioBuffer<float>(sidechainChannels, maximum);
            sidechain = cell::AudioBuffer<float>(sidechainChannels, maximum);
        }
        engine.prepare(rate, maximum, processorChannels);
        input_ = std::move(pull);
        output_ = std::move(wet);
        sidechainInput_ = std::move(sidechainPull);
        sidechain_ = std::move(sidechain);
        inputOrder_ = inputOrder;
        outputOrder_ = outputOrder;
        reset();
    }

    void cacheHostContext(AUHostMusicalContextBlock musical, AUHostTransportStateBlock transport,
                          bool offline, AUMIDIOutputEventBlock midiOutput) noexcept {
        musicalContext_ = musical;
        transportState_ = transport;
        offline_ = offline;
        midiOutput_ = midiOutput;
    }

    void reset() noexcept {
        cell::CallbackGate::AudioScope render(callbacks_);
        if (!render) return;
        automation_.reset();
        midiDelivery_.reset();
        for (std::size_t i = 0; i < fields().size(); ++i) {
            const auto pending = pending_[i].exchange(noPending, std::memory_order_acq_rel);
            if (std::isfinite(pending)) engine.parameters().set(fields()[i].id, pending);
        }
        if (engine.prepared()) engine.reset();
    }

    void release() noexcept {
#if defined(CELL_AUV3_EDITOR_METERS)
        meter.clear();
#endif
        cacheHostContext(nil, nil, false, nil);
        midiDelivery_.reset();
        automation_.reset();
        input_ = {};
        output_ = {};
        sidechainInput_ = {};
        sidechain_ = {};
    }

    AUAudioUnitStatus render(AudioUnitRenderActionFlags* flags, const AudioTimeStamp* time,
                             AUAudioFrameCount frames, NSInteger bus, AudioBufferList* output,
                             const AURenderEvent* event, AURenderPullInputBlock pullInput) noexcept {
        if (bus != 0) return kAudioUnitErr_InvalidElement;
        const bool midiOnly = engine.descriptor().channels == 0;
        if (!flags || !time || (!midiOnly && !output)) return kAudioUnitErr_InvalidPropertyValue;
        if (hasAudioInput && !pullInput) return kAudioUnitErr_NoConnection;
        if (frames > output_.frameCount()) return kAudioUnitErr_TooManyFramesToProcess;
        const auto channels = static_cast<UInt32>(output_.channelCount());
        const auto bytes = static_cast<UInt32>(frames * sizeof(float));
        const auto hostChannels = output ? output->mNumberBuffers : 0;
        if (hostChannels != channels && !(midiOnly && hostChannels == 0)) return kAudioUnitErr_FormatNotSupported;
        for (UInt32 c = 0; c < hostChannels; ++c) {
            const auto& buffer = output->mBuffers[c];
            if (buffer.mNumberChannels != 1 || (buffer.mData && buffer.mDataByteSize < bytes))
                return kAudioUnitErr_FormatNotSupported;
        }
        if (!frames) return noErr;

        cell::CallbackGate::AudioScope rendering(callbacks_);
        auto wet = output_.block().slice(0, frames);
        const bool dry = bypass.load(std::memory_order_relaxed);
        if (midiOnly) {
            if (rendering) {
                const auto status = processRenderEvents({.frames = frames}, {}, *time, event, dry);
                if (status != noErr) return status;
            }
            if (hostChannels) {
                wet.clear();
                writeOutput(*output, wet, bytes);
            }
            *flags |= kAudioUnitRenderAction_OutputIsSilence;
            return noErr;
        }
        if (!rendering) {
            wet.clear();
            writeOutput(*output, wet, bytes);
            *flags |= kAudioUnitRenderAction_OutputIsSilence;
            return noErr;
        }

        cell::AudioBlock<float> input;
        if (hasAudioInput) {
            input = input_.block().slice(0, frames);
            const auto mainStatus = pullBus(pullInput, *flags, time, 0, input, wet, inputOrder_);
            if (mainStatus != noErr) return mainStatus;
            if (dry)
                for (UInt32 c = 0; c < channels; ++c)
                    std::copy_n(wet.channel(c), frames, input.channel(c));
        } else {
            wet.clear();
        }

        cell::AudioBlock<float> sidechain;
        if (sidechain_.channelCount()) {
            sidechain = sidechain_.block().slice(0, frames);
            const auto sidechainStatus = pullBus(pullInput, *flags, time, 1,
                                               sidechainInput_.block().slice(0, frames), sidechain);
            if (sidechainStatus == kAudioUnitErr_NoConnection) sidechain = {};
            else if (sidechainStatus != noErr) return sidechainStatus;
        }

        const auto status = processRenderEvents(wet, sidechain, *time, event, dry);
        if (status != noErr) return status;
        if (dry && !hasAudioInput) {
            wet.clear();
            *flags |= kAudioUnitRenderAction_OutputIsSilence;
        } else {
            *flags &= ~kAudioUnitRenderAction_OutputIsSilence;
        }
        writeOutput(*output, dry && hasAudioInput ? input : wet, bytes);
        return noErr;
    }

    std::vector<std::byte> save() {
        cell::CallbackGate::PauseScope pause(callbacks_);
        if (!pause) return {};
        auto& bank = engine.parameters();
        struct RestoreValues {
            cell::ParameterBank& bank;
            std::vector<double> values;
            ~RestoreValues() {
                const auto fields = bank.descriptors();
                for (std::size_t i = 0; i < fields.size(); ++i)
                    bank.restorePlain(fields[i].id, values[i]);
            }
        } restore{bank, std::vector<double>(fields().size())};
        bank.copyValues(restore.values);
        for (const auto& field : fields()) bank.set(field.id, value(field.id));
        return engine.saveState();
    }

    void restore(std::span<const std::byte> bytes) {
        if (bytes.empty() || bytes.size() > maxStateBytes) return;
        cell::CallbackGate::PauseScope pause(callbacks_);
        if (!pause) return;
        std::vector<double> previous;
        previous.reserve(fields().size());
        for (std::size_t i = 0; i < fields().size(); ++i)
            previous.push_back(pending_[i].load(std::memory_order_acquire));
        if (!engine.loadState(bytes)) return;
        automation_.reset();
        midiDelivery_.reset();
        for (std::size_t i = 0; i < fields().size(); ++i)
            pending_[i].compare_exchange_strong(previous[i], noPending, std::memory_order_acq_rel);
    }

    cell::Engine engine;
    const bool hasAudioInput;
    std::atomic<bool> bypass{false};
#if defined(CELL_AUV3_EDITOR_METERS)
    cell::web::OutputMeter meter;
#endif

private:
    AUAudioUnitStatus processRenderEvents(cell::AudioBlock<float> audio, cell::AudioBlock<float> sidechain,
                                         const AudioTimeStamp& time, const AURenderEvent* event, bool dry) noexcept {
        const bool acceptsMidi = engine.descriptor().acceptsMidi;
        const bool producesMidi = engine.descriptor().producesMidi;
        std::size_t eventCount{};
        for (auto* item = event; item; item = item->head.next) {
            if (++eventCount > maxScheduled) {
                // The block is rejected, including any note-offs it carried.
                if (acceptsMidi || producesMidi) engine.reset();
                midiDelivery_.reset();
                return kAudioUnitErr_CannotDoInCurrentContext;
            }
        }
        events_.clear();
        midi_.clear();
        for (std::size_t i = 0; i < fields().size(); ++i) {
            const auto pending = pending_[i].exchange(noPending, std::memory_order_acq_rel);
            if (std::isfinite(pending)) events_.push_back({0, i, pending, 0});
        }
        for (; event; event = event->head.next) {
            const auto type = event->head.eventType;
            if (acceptsMidi && type == AURenderEventMIDI) {
                const auto frame = eventOffset(event->head.eventSampleTime, time);
                if (frame < audio.frames && (midi_.empty() || frame >= midi_.back().frame))
                    appendMidi(event->MIDI, frame);
                continue;
            }
            if (type != AURenderEventParameter && type != AURenderEventParameterRamp) continue;
            const auto index = parameterIndex(event->parameter.parameterAddress);
            if (index == fields().size() || !std::isfinite(event->parameter.value)) continue;
            const auto frame = eventOffset(event->head.eventSampleTime, time);
            if (frame >= audio.frames || (!events_.empty() && frame < events_.back().frame)) continue;
            const auto duration = type == AURenderEventParameterRamp ? event->parameter.rampDurationSampleFrames : 0;
            if (duration && !cell::auv3::canRamp(fields()[index])) continue;
            events_.push_back({frame, index, event->parameter.value, duration});
        }
        const auto transport = hostTransport(audio.frames);
        if (producesMidi)
            renderWithMidiOutput(audio, transport, sidechain, time, dry);
        else
            automation_.process(engine, audio, events_, transport, sidechain, midi_);
        return noErr;
    }

    void renderWithMidiOutput(cell::AudioBlock<float> audio, const cell::Transport& transport,
                              cell::AudioBlock<float> sidechain, const AudioTimeStamp& time, bool dry) noexcept {
        if (dry != midiBypassed_) {
            midiDelivery_.reset();
            midiBypassed_ = dry;
        }
        const auto start = time.mFlags & kAudioTimeStampSampleTimeValid
            ? samplePosition(time.mSampleTime, audio.frames) : std::nullopt;
        HostMidiOutput host(midiOutput_, start.value_or(0));
        midiDelivery_.begin(midiOutput_ && start ? &host : nullptr, audio.frames);
        DiscardMidiOutput discard;
        auto* output = dry ? static_cast<cell::midi::Output*>(&discard) : &midiDelivery_;
        automation_.process(engine, audio, events_, transport, sidechain, midi_, output);
        // Bypass keeps processing state current while routing only host input to MIDI output.
        if (dry) {
            for (auto event : midi_)
                if (!midiDelivery_.push(event)) break;
        }
    }

    void appendMidi(const AUMIDIEvent& event, UInt32 frame) noexcept {
        const auto size = midiMessageSize(event.data[0]);
        if (event.cable != 0 || event.length != size) return;
        const cell::MidiEvent midi{frame, {event.data[0], event.data[1], size == 3 ? event.data[2] : std::uint8_t{}}, size};
        if (cell::midi::valid(midi)) midi_.push_back(midi);
    }

    static AUAudioUnitStatus pullBus(AURenderPullInputBlock pullInput, AudioUnitRenderActionFlags flags,
                                    const AudioTimeStamp* time, NSInteger bus, cell::AudioBlock<float> scratch,
                                    cell::AudioBlock<float> audio,
                                    const auv3::ChannelOrder& order = auv3::identityOrder) noexcept {
        PlanarBuffers pull;
        pull.count = audio.channelCount;
        const auto bytes = static_cast<UInt32>(audio.frames * sizeof(float));
        for (UInt32 c = 0; c < pull.count; ++c) pull.buffers[c] = {1, bytes, scratch.channel(c)};
        flags &= ~kAudioUnitRenderAction_OutputIsSilence;
        const auto status = pullInput(&flags, time, audio.frames, bus, pull.list());
        if (status != noErr) return status;
        if (pull.count != audio.channelCount) return kAudioUnitErr_FormatNotSupported;
        const bool silent = (flags & kAudioUnitRenderAction_OutputIsSilence) != 0;
        for (UInt32 c = 0; c < pull.count; ++c) {
            const auto& channel = pull.buffers[order[c]];
            if (channel.mNumberChannels != 1 || (!silent && (!channel.mData || channel.mDataByteSize < bytes)))
                return kAudioUnitErr_FormatNotSupported;
            if (silent) std::fill_n(audio.channel(c), audio.frames, 0.0f);
            else std::memmove(audio.channel(c), channel.mData, bytes);
        }
        return noErr;
    }

    cell::Transport hostTransport(AUAudioFrameCount frames) const noexcept {
        constexpr double unavailable = std::numeric_limits<double>::quiet_NaN();
        cell::Transport transport;
        transport.offline = offline_;
        transport.tempoValid = false;

        double tempo = unavailable, beat = unavailable, numerator{};
        NSInteger denominator{};
        if (musicalContext_ && musicalContext_(&tempo, &numerator, &denominator, &beat, nullptr, nullptr)) {
            transport.tempoValid = std::isfinite(tempo) && tempo > 0;
            if (transport.tempoValid) transport.tempo = tempo;
            transport.beatPositionValid = std::isfinite(beat);
            if (transport.beatPositionValid) transport.beatPosition = beat;
            constexpr auto maxMeter = std::numeric_limits<std::uint16_t>::max();
            if (numerator >= 1 && numerator <= maxMeter && std::trunc(numerator) == numerator &&
                denominator >= 1 && denominator <= maxMeter) {
                transport.numerator = static_cast<std::uint16_t>(numerator);
                transport.denominator = static_cast<std::uint16_t>(denominator);
            }
        }

        AUHostTransportStateFlags flags{};
        double sample = unavailable, loopStart = unavailable, loopEnd = unavailable;
        if (transportState_ && transportState_(&flags, &sample, &loopStart, &loopEnd)) {
            transport.playing = (flags & AUHostTransportStateMoving) != 0;
            transport.recording = (flags & AUHostTransportStateRecording) != 0;
            transport.looping = (flags & AUHostTransportStateCycling) != 0;
            if (const auto position = samplePosition(sample, frames)) {
                transport.samplePosition = *position;
                transport.samplePositionValid = true;
            }
            transport.loopPointsValid = transport.looping && std::isfinite(loopStart) &&
                std::isfinite(loopEnd) && loopEnd > loopStart;
            if (transport.loopPointsValid) {
                transport.loopStartBeat = loopStart;
                transport.loopEndBeat = loopEnd;
            }
        }
        return transport;
    }

    std::size_t parameterIndex(AUParameterAddress address) const noexcept {
        if (address > std::numeric_limits<cell::ParamId>::max()) return fields().size();
        return engine.parameters().indexOf(static_cast<cell::ParamId>(address));
    }

    static UInt32 eventOffset(AUEventSampleTime sample, const AudioTimeStamp& time) noexcept {
        if (sample >= AUEventSampleTimeImmediate && sample < AUEventSampleTimeImmediate + 4096)
            return static_cast<UInt32>(sample - AUEventSampleTimeImmediate);
        if (!(time.mFlags & kAudioTimeStampSampleTimeValid) || !std::isfinite(time.mSampleTime))
            return UINT32_MAX;
        const auto offset = static_cast<double>(sample) - time.mSampleTime;
        if (offset <= 0) return 0;
        return offset < UINT32_MAX ? static_cast<UInt32>(offset) : UINT32_MAX;
    }

    void writeOutput(AudioBufferList& output, cell::AudioBlock<float> block, UInt32 bytes) noexcept {
#if defined(CELL_AUV3_EDITOR_METERS)
        meter.publish(block);
#endif
        for (UInt32 c = 0; c < block.channelCount; ++c) {
            auto& buffer = output.mBuffers[outputOrder_[c]];
            if (buffer.mData) std::memmove(buffer.mData, block.channel(c), bytes);
            else buffer.mData = block.channel(c);
            buffer.mDataByteSize = bytes;
        }
    }

    std::unique_ptr<std::atomic<double>[]> pending_;
    cell::AudioBuffer<float> input_, output_;
    cell::AudioBuffer<float> sidechainInput_, sidechain_;
    auv3::ChannelOrder inputOrder_ = auv3::identityOrder, outputOrder_ = auv3::identityOrder;
    std::vector<cell::auv3::AutomationEvent> events_;
    std::vector<cell::MidiEvent> midi_;
    cell::auv3::Automation automation_;
    cell::midi::Delivery midiDelivery_;
    cell::CallbackGate callbacks_;
    AUHostMusicalContextBlock musicalContext_ = nil;
    AUHostTransportStateBlock transportState_ = nil;
    AUMIDIOutputEventBlock midiOutput_ = nil;
    bool offline_{};
    bool midiBypassed_{};
};
} // namespace

@interface CellAUv3MainBus : AUAudioUnitBus
@property(nonatomic, copy) NSArray<NSNumber*>* layoutTags;
@end

@implementation CellAUv3MainBus
- (NSArray<NSNumber*>*)supportedChannelLayoutTags { return self.layoutTags; }
@end

@interface CellAUv3Effect : AUAudioUnit
#if defined(CELL_AUV3_EDITOR_METERS)
- (cell::web::OutputMeter*)editorMeter;
#endif
@end

@implementation CellAUv3Effect {
    std::shared_ptr<RenderState> _state;
    CellAUv3MainBus *_input;
    AUAudioUnitBus *_sidechain;
    CellAUv3MainBus *_output;
    AUAudioUnitBusArray *_inputs;
    AUAudioUnitBusArray *_outputs;
    AUParameterTree *_tree;
}

- (instancetype)initWithComponentDescription:(AudioComponentDescription)description
                                     options:(AudioComponentInstantiationOptions)options
                                       error:(NSError**)error {
    if (!(self = [super initWithComponentDescription:description options:options error:error])) return nil;
    const bool instrument = description.componentType == kAudioUnitType_MusicDevice;
    try { _state = std::make_shared<RenderState>(instrument); }
    catch (...) { reportError(error, kAudioUnitErr_FailedInitialization); return nil; }
    const auto& descriptor = _state->engine.descriptor();
    const bool midiOnly = descriptor.channels == 0;
    auto expectedType = kAudioUnitType_Effect;
    if (instrument) expectedType = kAudioUnitType_MusicDevice;
    else if (midiOnly) expectedType = kAudioUnitType_MIDIProcessor;
    else if (descriptor.acceptsMidi || descriptor.producesMidi) expectedType = kAudioUnitType_MusicEffect;
    if (description.componentType != expectedType) {
        reportError(error, kAudioUnitErr_FailedInitialization);
        return nil;
    }
    // A MIDI effect uses a silent output bus for the host's render clock.
    const auto layout = auv3::defaultMainLayout(descriptor, instrument).value_or(AudioLayout::stereo);
    const auto tag = std::find_if(std::begin(layoutTags), std::end(layoutTags),
                                 [layout](auto entry) { return entry.layout == layout; })->tag;
    AVAudioChannelLayout* channelLayout = [AVAudioChannelLayout layoutWithLayoutTag:tag];
    if (!channelLayout) {
        reportError(error, kAudioUnitErr_FormatNotSupported);
        return nil;
    }
    AVAudioFormat* format = [[AVAudioFormat alloc] initStandardFormatWithSampleRate:48000 channelLayout:channelLayout];
    if (_state->hasAudioInput) {
        _input = [[CellAUv3MainBus alloc] initWithFormat:format error:error];
        if (!_input) return nil;
    }
    _output = [[CellAUv3MainBus alloc] initWithFormat:format error:error];
    if (!_output) return nil;
    NSMutableArray<NSNumber*>* mainCounts = [NSMutableArray array];
    NSMutableArray<NSNumber*>* tags = [NSMutableArray array];
    const auto supported = [&](AudioLayout candidate) {
        if (midiOnly) return candidate == AudioLayout::mono || candidate == AudioLayout::stereo;
        return auv3::supportsMainLayout(descriptor, candidate, instrument);
    };
    for (auto candidate : auv3::mainLayouts)
        if (supported(candidate)) [mainCounts addObject:@(cell::channelCount(candidate))];
    for (const auto& entry : layoutTags)
        if (supported(entry.layout)) [tags addObject:@(entry.tag)];
    if (!descriptor.mainBusLayouts.empty()) {
        [tags addObject:@(kAudioChannelLayoutTag_UseChannelDescriptions)];
        [tags addObject:@(kAudioChannelLayoutTag_UseChannelBitmap)];
    }
    _input.supportedChannelCounts = mainCounts;
    _output.supportedChannelCounts = mainCounts;
    _input.layoutTags = tags;
    _output.layoutTags = tags;
    NSArray<AUAudioUnitBus*>* inputBusses = _input ? @[_input] : @[];
    if (descriptor.supportsSidechain) {
        AVAudioFormat* stereo = [[AVAudioFormat alloc] initStandardFormatWithSampleRate:48000 channels:2];
        _sidechain = [[AUAudioUnitBus alloc] initWithFormat:stereo error:error];
        if (!_sidechain) return nil;
        _sidechain.supportedChannelCounts = @[@1, @2];
        inputBusses = @[_input, _sidechain];
    }
    _inputs = [[AUAudioUnitBusArray alloc] initWithAudioUnit:self busType:AUAudioUnitBusTypeInput busses:inputBusses];
    _outputs = [[AUAudioUnitBusArray alloc] initWithAudioUnit:self busType:AUAudioUnitBusTypeOutput busses:@[_output]];
    _sidechain.name = @"Sidechain";
    _sidechain.enabled = NO;

    NSMutableArray<AUParameter*>* parameters = [NSMutableArray arrayWithCapacity:_state->fields().size()];
    for (const auto& field : _state->fields()) {
        NSString* identifier = [NSString stringWithUTF8String:field.key.c_str()];
        NSString* name = [NSString stringWithUTF8String:field.name.c_str()];
        const auto minimum = static_cast<float>(field.minimum);
        const auto maximum = static_cast<float>(field.maximum);
        if (!identifier || !name || !std::isfinite(minimum) || !std::isfinite(maximum) || minimum >= maximum) {
            reportError(error, kAudioUnitErr_InvalidParameter);
            return nil;
        }
        auto flags = kAudioUnitParameterFlag_IsReadable | kAudioUnitParameterFlag_IsWritable;
        if (!field.automatable) flags |= kAudioUnitParameterFlag_NonRealTime;
        if (cell::auv3::canRamp(field)) flags |= kAudioUnitParameterFlag_CanRamp;
        auto* parameter = [AUParameterTree createParameterWithIdentifier:identifier name:name address:field.id
            min:minimum max:maximum unit:unitFor(field) unitName:nil flags:flags valueStrings:nil dependentParameters:nil];
        parameter.value = static_cast<AUValue>(_state->value(field.id));
        [parameters addObject:parameter];
    }
    _tree = [AUParameterTree createTreeWithChildren:parameters];
    const auto state = _state;
    _tree.implementorValueObserver = ^(AUParameter* parameter, AUValue value) {
        state->put(parameter.address, value);
    };
    _tree.implementorValueProvider = ^AUValue(AUParameter* parameter) {
        return static_cast<AUValue>(state->value(parameter.address));
    };
    return self;
}

- (AUAudioUnitBusArray*)inputBusses { return _inputs; }
- (AUAudioUnitBusArray*)outputBusses { return _outputs; }
- (AUParameterTree*)parameterTree { return _tree; }
#if defined(CELL_AUV3_EDITOR_METERS)
- (cell::web::OutputMeter*)editorMeter { return &_state->meter; }
#endif
- (BOOL)isMusicDeviceOrEffect {
    if (_state && _state->engine.descriptor().channels == 0 && _state->engine.descriptor().acceptsMidi) return YES;
    return [super isMusicDeviceOrEffect];
}
- (NSInteger)virtualMIDICableCount {
    return _state ? (_state->engine.descriptor().acceptsMidi ? 1 : 0) : [super virtualMIDICableCount];
}
- (NSArray<NSString*>*)MIDIOutputNames {
    return _state && _state->engine.descriptor().producesMidi ? @[@"MIDI Output"] : @[];
}
- (NSArray<NSNumber*>*)channelCapabilities {
    if (!_state) return [super channelCapabilities];
    const auto& descriptor = _state->engine.descriptor();
    if (descriptor.channels == 0) return @[@0, @2, @0, @1];
    NSMutableArray<NSNumber*>* capabilities = [NSMutableArray array];
    // Report the default first for hosts that choose one layout from this list.
    const bool outputOnly = !_state->hasAudioInput;
    NSNumber* preferred = @(cell::channelCount(*auv3::defaultMainLayout(descriptor, outputOnly)));
    [capabilities addObjectsFromArray:@[outputOnly ? @0 : preferred, preferred]];
    for (NSNumber* count in _output.supportedChannelCounts)
        if (![count isEqual:preferred]) [capabilities addObjectsFromArray:@[outputOnly ? @0 : count, count]];
    return capabilities;
}
- (BOOL)shouldChangeToFormat:(AVAudioFormat*)format forBus:(AUAudioUnitBus*)bus {
    if (![super shouldChangeToFormat:format forBus:bus]) return NO;
    if (bus == _sidechain || _state->engine.descriptor().channels == 0) return supportsMonoStereoPCM(format);
    return mainOrder(format, _state->engine.descriptor(), !_state->hasAudioInput).has_value();
}
- (BOOL)allocateRenderResourcesAndReturnError:(NSError**)error {
    const auto& descriptor = _state->engine.descriptor();
    const bool midiOnly = descriptor.channels == 0;
    auto inputOrder = std::optional{auv3::identityOrder};
    auto outputOrder = inputOrder;
    if (midiOnly) {
        if (!supportsMonoStereoPCM(_output.format)) outputOrder.reset();
    } else {
        if (_input) inputOrder = mainOrder(_input.format, descriptor);
        outputOrder = mainOrder(_output.format, descriptor, !_input);
    }
    const bool sidechainEnabled = _sidechain.isEnabled;
    AVAudioFormat* sidechainFormat = sidechainEnabled ? _sidechain.format : nil;
    if (!inputOrder || !outputOrder ||
        (_input && (_input.format.sampleRate != _output.format.sampleRate ||
            _input.format.channelCount != _output.format.channelCount)) ||
        (sidechainEnabled && (!supportsMonoStereoPCM(sidechainFormat) || sidechainFormat.sampleRate != _output.format.sampleRate)) ||
        self.maximumFramesToRender == 0 || self.maximumFramesToRender > (1u << 20)) {
        reportError(error, kAudioUnitErr_FormatNotSupported);
        return NO;
    }
    if (![super allocateRenderResourcesAndReturnError:error]) return NO;
    try {
        _state->prepare(_output.format.sampleRate, self.maximumFramesToRender, _output.format.channelCount,
                        sidechainFormat.channelCount, *inputOrder, *outputOrder);
        AUMIDIOutputEventBlock midiOutput = nil;
        if (_state->engine.descriptor().producesMidi) midiOutput = self.MIDIOutputEventBlock;
        _state->cacheHostContext(self.musicalContextBlock, self.transportStateBlock, self.isRenderingOffline, midiOutput);
    } catch (...) {
        [super deallocateRenderResources];
        reportError(error, kAudioUnitErr_FailedInitialization);
        return NO;
    }
    return YES;
}
- (void)reset { [super reset]; _state->reset(); }
- (void)deallocateRenderResources { [super deallocateRenderResources]; _state->release(); }
- (BOOL)shouldBypassEffect { return _state->bypass.load(std::memory_order_relaxed); }
- (void)setShouldBypassEffect:(BOOL)bypass { _state->bypass.store(bypass, std::memory_order_relaxed); }
- (NSTimeInterval)latency { return _state->engine.processor().latency() / _output.format.sampleRate; }
- (NSTimeInterval)tailTime { return _state->engine.processor().tail() / _output.format.sampleRate; }

- (NSDictionary<NSString*, id>*)fullState {
    try {
        const auto bytes = _state->save();
        if (bytes.empty() || bytes.size() > maxStateBytes) return nil;
        NSMutableDictionary* state = [[super fullState] mutableCopy] ?: [NSMutableDictionary dictionary];
        state[@"Cell.EngineState"] = [NSData dataWithBytes:bytes.data() length:bytes.size()];
        return state;
    } catch (...) { return nil; }
}
- (void)setFullState:(NSDictionary<NSString*, id>*)state {
    id payload = state[@"Cell.EngineState"];
    if (![payload isKindOfClass:[NSData class]]) return;
    NSData* data = payload;
    try { _state->restore({static_cast<const std::byte*>(data.bytes), data.length}); }
    catch (...) {}
}

- (AUInternalRenderBlock)internalRenderBlock {
    const auto state = _state;
    return ^AUAudioUnitStatus(AudioUnitRenderActionFlags* flags, const AudioTimeStamp* time,
                              AUAudioFrameCount frames, NSInteger bus, AudioBufferList* output,
                              const AURenderEvent* event, __unsafe_unretained AURenderPullInputBlock pullInput) {
        return state->render(flags, time, frames, bus, output, event, pullInput);
    };
}
@end

#if defined(CELL_AUV3_CUSTOM_EDITOR)
AUAudioUnit* cellCreateAUv3Unit(AudioComponentDescription description, NSError** error) {
    return [[CellAUv3Effect alloc] initWithComponentDescription:description options:0 error:error];
}
#if defined(CELL_AUV3_EDITOR_METERS)
cell::web::OutputMeter* cellAUv3EditorMeter(AUAudioUnit* unit) {
    return [(CellAUv3Effect*)unit editorMeter];
}
#endif
#else
@interface CellAUv3Factory : NSObject<AUAudioUnitFactory>
@end

@implementation CellAUv3Factory
- (void)beginRequestWithExtensionContext:(NSExtensionContext*)context { (void)context; }
- (AUAudioUnit*)createAudioUnitWithComponentDescription:(AudioComponentDescription)description error:(NSError**)error {
    return [[CellAUv3Effect alloc] initWithComponentDescription:description options:0 error:error];
}
@end
#endif
