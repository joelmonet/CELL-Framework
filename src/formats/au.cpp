// Original Cell Framework implementation; see LICENSE.
#include <AudioUnitSDK/AUEffectBase.h>
#include <cell/callback_gate.hpp>
#include <cell/plugin.hpp>
#if defined(CELL_AU_WEB_EDITOR)
#include "au_editor.hpp"
#endif

namespace {

constexpr std::size_t maxStateBytes = 128 * 1024 * 1024;

bool componentField(CFDictionaryRef state, CFStringRef key, OSType expected) {
    const auto field = static_cast<CFNumberRef>(CFDictionaryGetValue(state, key));
    SInt64 value{};
    return field && CFGetTypeID(field) == CFNumberGetTypeID() &&
        CFNumberGetValue(field, kCFNumberSInt64Type, &value) && value == expected;
}

AudioUnitParameterUnit parameterUnit(const cell::Parameter& parameter) {
    if (parameter.unit == "dB") return kAudioUnitParameterUnit_Decibels;
    if (parameter.unit == "Hz") return kAudioUnitParameterUnit_Hertz;
    if (parameter.unit == "%") return kAudioUnitParameterUnit_Percent;
    return kAudioUnitParameterUnit_Generic;
}

class CellAudioUnit final : public ausdk::AUEffectBase {
public:
    explicit CellAudioUnit(AudioComponentInstance instance)
        : AUEffectBase(instance, false),
          engine_(cell::plugin::descriptor(), cell::plugin::createProcessor()) {}

    void PostConstructor() override {
        const auto& descriptor = engine_.descriptor();
        if (descriptor.channels != 2 || descriptor.supportsSidechain || descriptor.acceptsMidi ||
            descriptor.producesMidi || !descriptor.mainBusLayouts.empty())
            throw std::invalid_argument("Cell AU requires a fixed stereo effect with optional mono");
        for (const auto& parameter : engine_.parameters().descriptors()) {
            if (parameter.minimum < -std::numeric_limits<float>::max() ||
                parameter.maximum > std::numeric_limits<float>::max() ||
                static_cast<float>(parameter.minimum) >= static_cast<float>(parameter.maximum))
                throw std::invalid_argument("Cell AU requires a representable float parameter range");
            Globals()->SetParameter(parameter.id, static_cast<float>(engine_.parameters().get(parameter.id)));
        }
        events_.reserve(engine_.parameters().descriptors().size());
#if defined(CELL_AU_WEB_EDITOR)
        editor_ = cell::au::registerEditorConnection(GetComponentInstance());
        editor_->programs = [this]() -> std::optional<cell::ProgramCatalog> {
            if (!dynamic_cast<const cell::ProgramMemory*>(&engine_.processor())) return {};
            const auto fields = engine_.parameters().descriptors();
            cell::ParameterBank values(fields);
            for (const auto& field : fields) values.set(field.id, GetParameterRT(field.id));
            return cell::web::detail::processorPrograms(engine_, values);
        };
        editor_->supportsAction = [this](std::string_view key, cell::web::ActionMode mode) {
            return cell::web::detail::processorSupportsAction(engine_, key, mode);
        };
        editor_->command = [this](const std::function<bool(cell::Engine&)>& operation, std::vector<cell::ParameterEdit>& edits) {
            cell::CallbackGate::PauseScope pause(callbacks_);
            if (!pause) return false;
            synchronizeParameters();
            const bool accepted = operation(engine_);
            for (cell::ParameterEdit edit; engine_.pollUiEdit(edit);) {
                if (edit.type == cell::EditType::value) Globals()->SetParameter(edit.id, static_cast<float>(edit.value));
                edits.push_back(edit);
            }
            return accepted;
        };
#endif
    }

#if defined(CELL_AU_WEB_EDITOR)
    void PreDestructor() override { cell::au::unregisterEditorConnection(GetComponentInstance()); }

    OSStatus GetPropertyInfo(AudioUnitPropertyID id, AudioUnitScope scope, AudioUnitElement element,
                             UInt32& size, bool& writable) override {
        if (id != kAudioUnitProperty_CocoaUI) return AUEffectBase::GetPropertyInfo(id, scope, element, size, writable);
        if (scope != kAudioUnitScope_Global) return kAudioUnitErr_InvalidScope;
        if (element != 0) return kAudioUnitErr_InvalidElement;
        size = sizeof(AudioUnitCocoaViewInfo);
        writable = false;
        return noErr;
    }

    OSStatus GetProperty(AudioUnitPropertyID id, AudioUnitScope scope, AudioUnitElement element, void* data) override {
        if (id != kAudioUnitProperty_CocoaUI) return AUEffectBase::GetProperty(id, scope, element, data);
        if (scope != kAudioUnitScope_Global) return kAudioUnitErr_InvalidScope;
        if (element != 0) return kAudioUnitErr_InvalidElement;
        if (!data) return kAudioUnitErr_InvalidPropertyValue;
        return cell::au::copyCocoaViewInfo(*static_cast<AudioUnitCocoaViewInfo*>(data));
    }
#endif

    UInt32 SupportedNumChannels(const AUChannelInfo** info) override {
        static constexpr AUChannelInfo layouts[]{{2, 2}, {1, 1}};
        if (info) *info = layouts;
        return engine_.descriptor().supportsMono ? 2 : 1;
    }

    bool ValidFormat(AudioUnitScope scope, AudioUnitElement element,
                     const AudioStreamBasicDescription& format) override {
        return AUEffectBase::ValidFormat(scope, element, format) &&
            (format.mChannelsPerFrame == 2 || (format.mChannelsPerFrame == 1 && engine_.descriptor().supportsMono));
    }

    OSStatus Initialize() override {
        const auto rate = GetSampleRate();
        if (GetInput0().GetStreamFormat().mSampleRate != rate)
            return kAudioUnitErr_FormatNotSupported;
        const auto status = AUEffectBase::Initialize();
        if (status != noErr) return status;
        const auto channels = GetNumberOfChannels();
        const auto frames = GetMaxFramesPerSlice();
        try {
            synchronizeParameters();
            engine_.prepare(rate, frames, channels);
            scratch_.resize(channels, frames);
        } catch (...) {
            AUEffectBase::Cleanup();
            return kAudioUnitErr_FailedInitialization;
        }
        return noErr;
    }

    OSStatus Reset(AudioUnitScope scope, AudioUnitElement element) override {
        cell::CallbackGate::AudioScope render(callbacks_);
        if (!render) return kAudioUnitErr_CannotDoInCurrentContext;
        synchronizeParameters();
        if (engine_.prepared()) engine_.reset();
        return AUEffectBase::Reset(scope, element);
    }

    // The base's scheduled-event path uses a growable queue. This adapter
    // publishes ordinary AU parameters and samples their latest values per render.
    bool CanScheduleParameters() const AUSDK_RTSAFE override { return false; }

    OSStatus SetParameter(AudioUnitParameterID id, AudioUnitScope scope, AudioUnitElement element,
                          AudioUnitParameterValue value, UInt32 offset) AUSDK_RTSAFE override {
        if (scope != kAudioUnitScope_Global || element != 0)
            return kAudioUnitErr_InvalidScope;
        const auto* parameter = engine_.parameters().find(id);
        if (!parameter || !std::isfinite(value)) return kAudioUnitErr_InvalidParameter;
        return AUEffectBase::SetParameter(id, scope, element, static_cast<float>(parameter->clamp(value)), offset);
    }

    OSStatus GetParameterInfo(AudioUnitScope scope, AudioUnitParameterID id,
                              AudioUnitParameterInfo& info) override {
        if (scope != kAudioUnitScope_Global) return kAudioUnitErr_InvalidScope;
        const auto* parameter = engine_.parameters().find(id);
        if (!parameter) return kAudioUnitErr_InvalidParameter;
        info = {};
        info.flags = kAudioUnitParameterFlag_IsReadable | kAudioUnitParameterFlag_IsWritable;
        if (!parameter->automatable) info.flags |= kAudioUnitParameterFlag_NonRealTime;
        info.unit = parameterUnit(*parameter);
        info.minValue = static_cast<float>(parameter->minimum);
        info.maxValue = static_cast<float>(parameter->maximum);
        info.defaultValue = static_cast<float>(parameter->defaultValue);
        const auto name = CFStringCreateWithCString(nullptr, parameter->name.c_str(), kCFStringEncodingUTF8);
        if (!name) return kAudioUnitErr_InvalidParameter;
        FillInParameterName(info, name, true);
        return noErr;
    }

    Float64 GetLatency() AUSDK_RTSAFE override { return engine_.processor().latency() / GetSampleRate(); }
    Float64 GetTailTime() AUSDK_RTSAFE override { return engine_.processor().tail() / GetSampleRate(); }
    bool SupportsTail() AUSDK_RTSAFE override { return true; }

#if defined(CELL_AU_EDITOR_METERS)
    void Cleanup() override {
        editor_->meter.clear();
        AUEffectBase::Cleanup();
    }

    OSStatus Render(AudioUnitRenderActionFlags& flags, const AudioTimeStamp& timestamp,
                    UInt32 frames) AUSDK_RTSAFE override {
        const auto status = AUEffectBase::Render(flags, timestamp, frames);
        if (editor_->meterReaders.load(std::memory_order_relaxed) == 0) return status;
        if (status != noErr || !frames || (flags & kAudioUnitRenderAction_OutputIsSilence)) {
            editor_->meter.clear();
            return status;
        }
        const auto output = GetOutput0().GetBufferListOrError();
        const auto channels = GetNumberOfChannels();
        if (!output || channels > 2 || !validBuffers(*output, channels, frames, false)) {
            editor_->meter.clear();
            return status;
        }
        // The rendered output also covers Apple's built-in bypass path.
        std::array<float*, 2> samples{};
        for (UInt32 channel = 0; channel < channels; ++channel)
            samples[channel] = static_cast<float*>(output->mBuffers[channel].mData);
        editor_->meter.publish({samples.data(), channels, frames, 0});
        return status;
    }
#endif

    OSStatus ProcessBufferLists(AudioUnitRenderActionFlags& flags, const AudioBufferList& input,
                               AudioBufferList& output, UInt32 frames) AUSDK_RTSAFE override {
        if (frames == 0) return noErr;
        const auto channels = GetNumberOfChannels();
        const auto inputSilent = (flags & kAudioUnitRenderAction_OutputIsSilence) != 0;
        if (frames > GetMaxFramesPerSlice() || frames > scratch_.frameCount())
            return kAudioUnitErr_TooManyFramesToProcess;
        if (!validBuffers(output, channels, frames, false) || !validBuffers(input, channels, frames, inputSilent))
            return kAudioUnitErr_InvalidPropertyValue;

        cell::CallbackGate::AudioScope render(callbacks_);
        if (!render) {
            for (UInt32 channel = 0; channel < channels; ++channel)
                std::fill_n(static_cast<float*>(output.mBuffers[channel].mData), frames, 0.0f);
            flags |= kAudioUnitRenderAction_OutputIsSilence;
            return noErr;
        }

        auto block = scratch_.block().slice(0, frames);
        for (UInt32 channel = 0; channel < channels; ++channel) {
            if (inputSilent) std::fill_n(block.channel(channel), frames, 0.0f);
            else std::copy_n(static_cast<const float*>(input.mBuffers[channel].mData), frames, block.channel(channel));
        }
        events_.clear();
        for (const auto& parameter : engine_.parameters().descriptors()) {
            const auto value = parameter.clamp(GetParameterRT(parameter.id));
            if (value != engine_.parameters().get(parameter.id)) events_.push_back({0, parameter.id, value});
        }
        engine_.process(block, events_, hostTransport());
        for (UInt32 channel = 0; channel < channels; ++channel)
            std::copy_n(block.channel(channel), frames, static_cast<float*>(output.mBuffers[channel].mData));
        flags &= ~kAudioUnitRenderAction_OutputIsSilence;
        return noErr;
    }

    OSStatus SaveState(CFPropertyListRef* output) override {
        cell::CallbackGate::PauseScope pause(callbacks_);
        if (!pause) return kAudioUnitErr_CannotDoInCurrentContext;
        const auto bytes = snapshotState();
        if (bytes.empty() || bytes.size() > maxStateBytes) return kAudioUnitErr_InvalidPropertyValue;
        CFPropertyListRef raw{};
        const auto status = AUEffectBase::SaveState(&raw);
        if (status != noErr) return status;
        const auto original = ausdk::Owned<CFPropertyListRef>::from_create(raw);
        auto state = ausdk::Owned<CFMutableDictionaryRef>::from_create(
            CFDictionaryCreateMutableCopy(nullptr, 0, static_cast<CFDictionaryRef>(*original)));
        const auto payload = ausdk::Owned<CFDataRef>::from_create(
            CFDataCreate(nullptr, reinterpret_cast<const UInt8*>(bytes.data()), static_cast<CFIndex>(bytes.size())));
        if (!*state || !*payload) return kAudioUnitErr_InvalidPropertyValue;
        CFDictionarySetValue(*state, CFSTR("Cell.EngineState"), *payload);
        *output = state.release();
        return noErr;
    }

    OSStatus RestoreState(CFPropertyListRef input) override {
        if (!input || CFGetTypeID(input) != CFDictionaryGetTypeID()) return kAudioUnitErr_InvalidPropertyValue;
        const auto state = static_cast<CFDictionaryRef>(input);
        const auto component = GetComponentDescription();
        if (!componentField(state, CFSTR(kAUPresetVersionKey), 0) ||
            !componentField(state, CFSTR(kAUPresetTypeKey), component.componentType) ||
            !componentField(state, CFSTR(kAUPresetSubtypeKey), component.componentSubType) ||
            !componentField(state, CFSTR(kAUPresetManufacturerKey), component.componentManufacturer))
            return kAudioUnitErr_InvalidPropertyValue;
        const auto data = static_cast<CFDataRef>(CFDictionaryGetValue(state, CFSTR("Cell.EngineState")));
        if (!data || CFGetTypeID(data) != CFDataGetTypeID()) return kAudioUnitErr_InvalidPropertyValue;
        const auto bytes = CFDataGetLength(data);
        if (bytes <= 0 || static_cast<std::size_t>(bytes) > maxStateBytes)
            return kAudioUnitErr_InvalidPropertyValue;
        cell::CallbackGate::PauseScope pause(callbacks_);
        if (!pause) return kAudioUnitErr_CannotDoInCurrentContext;
        if (!engine_.loadState({reinterpret_cast<const std::byte*>(CFDataGetBytePtr(data)),
                                static_cast<std::size_t>(bytes)}))
            return kAudioUnitErr_InvalidPropertyValue;
        for (const auto& parameter : engine_.parameters().descriptors())
            Globals()->SetParameter(parameter.id, static_cast<float>(engine_.parameters().get(parameter.id)));
        return noErr;
    }

private:
    static bool validBuffers(const AudioBufferList& buffers, UInt32 channels, UInt32 frames, bool silent) noexcept {
        if (buffers.mNumberBuffers != channels) return false;
        for (UInt32 channel = 0; channel < channels; ++channel) {
            const auto& buffer = buffers.mBuffers[channel];
            if (buffer.mNumberChannels != 1 ||
                (!silent && (!buffer.mData || buffer.mDataByteSize < frames * sizeof(float)))) return false;
        }
        return true;
    }

    void synchronizeParameters() noexcept {
        for (const auto& parameter : engine_.parameters().descriptors())
            engine_.parameters().set(parameter.id, GetParameterRT(parameter.id));
    }

    std::vector<std::byte> snapshotState() {
        auto& bank = engine_.parameters();
        const auto fields = bank.descriptors();
        // Capture host edits that arrived since the last render. Restore the
        // live bank so the next render still delivers their parameter events.
        struct RestoreBank {
            cell::ParameterBank& bank;
            std::vector<double> values;
            ~RestoreBank() {
                const auto fields = bank.descriptors();
                for (std::size_t i = 0; i < fields.size(); ++i)
                    bank.restorePlain(fields[i].id, values[i]);
            }
        } restore{bank, std::vector<double>(fields.size())};
        bank.copyValues(restore.values);
        synchronizeParameters();
        return engine_.saveState();
    }

    cell::Transport hostTransport() const noexcept {
        cell::Transport transport;
        double beat{}, tempo{};
        const bool hasTempo = CallHostBeatAndTempo(&beat, &tempo) == noErr;
        transport.tempoValid = hasTempo && std::isfinite(tempo) && tempo > 0;
        if (transport.tempoValid) transport.tempo = tempo;
        transport.beatPositionValid = hasTempo && std::isfinite(beat);
        if (transport.beatPositionValid) transport.beatPosition = beat;
        Boolean playing{}, cycling{};
        double currentSample{}, loopStart{}, loopEnd{};
        if (CallHostTransportState(&playing, nullptr, &currentSample, &cycling, &loopStart, &loopEnd) == noErr) {
            transport.playing = playing;
            transport.looping = cycling;
            transport.samplePositionValid = std::isfinite(currentSample) && currentSample >= -0x1p63 && currentSample < 0x1p63;
            if (transport.samplePositionValid) transport.samplePosition = static_cast<std::int64_t>(currentSample);
            transport.loopPointsValid = cycling && std::isfinite(loopStart) && std::isfinite(loopEnd) && loopEnd > loopStart;
            if (transport.loopPointsValid) {
                transport.loopStartBeat = loopStart;
                transport.loopEndBeat = loopEnd;
            }
        }
        return transport;
    }

    cell::Engine engine_;
    cell::AudioBuffer<float> scratch_;
    std::vector<cell::ParameterEvent> events_;
    cell::CallbackGate callbacks_;
#if defined(CELL_AU_WEB_EDITOR)
    std::shared_ptr<cell::au::EditorConnection> editor_;
#endif
};

} // namespace

AUSDK_COMPONENT_ENTRY(ausdk::AUBaseFactory, CellAudioUnit)
