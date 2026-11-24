// Independently written VST2.4 host adapter. The actual, separately supplied
// SDK defines the ABI; Cell does not redistribute replacement SDK headers.
#include <cell/text.hpp>
#include <cell/commands.hpp>
#include <cell/wrapper.hpp>
#include "pluginterfaces/vst2.x/aeffectx.h"
#if CELL_HAS_UI
#include <cell/ui.hpp>
#endif
#include <charconv>
#include <cstring>
#include <limits>

#ifndef CELL_VST2_UNIQUE_ID
#error "Supply the product's explicit VST2 unique ID; never derive it from a display name"
#endif
#ifndef CELL_PLUGIN_VERSION_CODE
#define CELL_PLUGIN_VERSION_CODE 10000
#endif

namespace {
using namespace cell;
using wrapper::Phase;
using wrapper::Result;

void copy(char* destination, std::size_t capacity, const char* value) noexcept {
    if (!destination || !capacity) return;
    std::strncpy(destination, value ? value : "", capacity - 1); destination[capacity - 1] = 0;
}

struct Instance {
    AEffect effect{};
    audioMasterCallback host{};
    wrapper::Runtime runtime{plugin::descriptor(), plugin::createProcessor()};
    double sampleRate = 48000;
    std::uint32_t blockSize = 512, channels = plugin::descriptor().channels;
    std::array<MidiEvent, wrapper::Limits::midi> midi{};
    std::size_t midiCount{};
    bool midiOverflow{};
    std::vector<std::byte> chunk;
    std::mutex chunkMutex;
    VstSpeakerArrangement inputArrangement{}, outputArrangement{};
#if CELL_HAS_UI
    std::unique_ptr<ui::Editor> editor;
    std::unique_ptr<ui::Window> window;
    ERect rectangle{};
#endif

    explicit Instance(audioMasterCallback callback) : host(callback) {
        const auto& info = runtime.engine().descriptor();
        if ((channels != 1 && channels != 2) || !CELL_VST2_UNIQUE_ID ||
            info.parameters.size() > std::size_t(std::numeric_limits<VstInt32>::max()))
            throw std::invalid_argument("VST2 adapter requires mono/stereo and a valid explicit identity");
        effect.magic = kEffectMagic; effect.object = this;
        effect.dispatcher = dispatch; effect.setParameter = setParameter; effect.getParameter = getParameter;
        effect.processReplacing = process;
        effect.numPrograms = 1; effect.numParams = VstInt32(info.parameters.size());
        effect.numInputs = VstInt32(channels + (info.supportsSidechain ? 2 : 0)); effect.numOutputs = VstInt32(channels);
        effect.flags = effFlagsCanReplacing | effFlagsProgramChunks;
#if CELL_HAS_UI
        effect.flags |= effFlagsHasEditor;
#endif
        effect.uniqueID = VstInt32(CELL_VST2_UNIQUE_ID); effect.version = CELL_PLUGIN_VERSION_CODE;
        inputArrangement.type = outputArrangement.type = channels == 1 ? kSpeakerArrMono : kSpeakerArrStereo;
        inputArrangement.numChannels = outputArrangement.numChannels = VstInt32(channels);
    }

    static Instance* get(AEffect* effect) noexcept { return effect ? static_cast<Instance*>(effect->object) : nullptr; }
    VstIntPtr call(VstInt32 opcode, VstInt32 index = 0, VstIntPtr value = 0, void* pointer = nullptr, float option = 0) noexcept {
        return host ? host(&effect, opcode, index, value, pointer, option) : 0;
    }
    const Parameter* parameter(VstInt32 index) const noexcept {
        const auto all = runtime.engine().parameters().descriptors();
        return index >= 0 && std::size_t(index) < all.size() ? &all[std::size_t(index)] : nullptr;
    }
    static void VSTCALLBACK setParameter(AEffect* effect, VstInt32 index, float value) noexcept {
        if (auto* instance = get(effect)) if (const auto* parameter = instance->parameter(index); parameter && std::isfinite(value))
            instance->runtime.engine().parameters().set(parameter->id, parameter->fromNormalized(value));
    }
    static float VSTCALLBACK getParameter(AEffect* effect, VstInt32 index) noexcept {
        if (auto* instance = get(effect)) if (const auto* parameter = instance->parameter(index))
            return float(parameter->toNormalized(instance->runtime.engine().parameters().get(parameter->id)));
        return 0;
    }
    void sendEdit(const ParameterEdit& edit) noexcept {
        const auto index = runtime.engine().parameters().indexOf(edit.id);
        const auto* field = parameter(VstInt32(index)); if (!field) return;
        if (edit.type == EditType::begin) call(audioMasterBeginEdit, VstInt32(index));
        else if (edit.type == EditType::end) call(audioMasterEndEdit, VstInt32(index));
        else call(audioMasterAutomate, VstInt32(index), 0, nullptr, float(field->toNormalized(edit.value)));
    }
    void uiEdits() noexcept {
        ParameterEdit edit; while (runtime.engine().pollUiEdit(edit)) sendEdit(edit);
    }
    void refreshLatency() noexcept {
        const auto latency = runtime.engine().processor().latency();
        const auto clamped = VstInt32(std::min(latency, std::uint32_t(std::numeric_limits<VstInt32>::max())));
        if (clamped != effect.initialDelay) { effect.initialDelay = clamped; call(audioMasterIOChanged); }
    }
    bool mains(bool enabled) noexcept {
        auto phase = runtime.phase();
        if (enabled) {
            if (phase == Phase::processing) return true;
            if (phase == Phase::unprepared || phase == Phase::ready) {
                if (runtime.configure({sampleRate, blockSize, channels}) != Result::ok || runtime.activate() != Result::ok) return false;
            }
            if (runtime.start() != Result::ok) return false;
            refreshLatency(); return true;
        }
        if (phase == Phase::processing && runtime.stop() != Result::ok) return false;
        phase = runtime.phase();
        if (phase == Phase::active && runtime.deactivate() != Result::ok) return false;
        midiCount = 0; midiOverflow = false;
        return runtime.phase() == Phase::ready || runtime.phase() == Phase::unprepared;
    }
    Transport transport() noexcept {
        Transport result;result.tempoValid=false;
        constexpr VstInt32 requested = kVstPpqPosValid | kVstTempoValid | kVstTimeSigValid;
        if (const auto* time = reinterpret_cast<const VstTimeInfo*>(call(audioMasterGetTime, 0, requested))) {
            result.playing = (time->flags & kVstTransportPlaying) != 0;
            result.recording = (time->flags & kVstTransportRecording) != 0;
            result.looping = (time->flags & kVstTransportCycleActive) != 0;
            if (std::isfinite(time->samplePos) && time->samplePos > double(std::numeric_limits<std::int64_t>::min()) &&
                time->samplePos < double(std::numeric_limits<std::int64_t>::max())){result.samplePosition = std::int64_t(time->samplePos);result.samplePositionValid=true;}
            if ((time->flags & kVstPpqPosValid) && std::isfinite(time->ppqPos)) {result.beatPosition = time->ppqPos;result.beatPositionValid = true;}
            if ((time->flags & kVstTempoValid) && std::isfinite(time->tempo) && time->tempo > 0){result.tempo = time->tempo;result.tempoValid=true;}
            if ((time->flags & kVstTimeSigValid) && time->timeSigNumerator > 0 && time->timeSigNumerator <= 65535 &&
                time->timeSigDenominator > 0 && time->timeSigDenominator <= 65535) {
                result.numerator = std::uint16_t(time->timeSigNumerator); result.denominator = std::uint16_t(time->timeSigDenominator);
            }
        }
        result.offline = call(audioMasterGetCurrentProcessLevel) == kVstProcessLevelOffline;
        return result;
    }
    static void VSTCALLBACK process(AEffect* effect, float** inputs, float** outputs, VstInt32 frames) noexcept {
        auto* instance = get(effect); if (!instance || frames < 0) return;
        auto& i = *instance;
        std::array<const float*, 2> main{}, side{};
        std::array<float*, 2> out{};
        for (unsigned channel = 0; channel < i.channels; ++channel) {
            main[channel] = inputs ? inputs[channel] : nullptr;
            out[channel] = outputs ? outputs[channel] : nullptr;
        }
        if (plugin::descriptor().supportsSidechain)
            for (unsigned channel = 0; channel < 2; ++channel) side[channel] = inputs ? inputs[i.channels + channel] : nullptr;
        if (i.midiOverflow) {
            i.runtime.requestMidiPanic();
            if (std::uint32_t(frames) <= wrapper::Limits::hostFrames)
                for (unsigned channel = 0; channel < i.channels; ++channel) if (out[channel]) std::fill_n(out[channel], frames, 0.f);
        } else {
            const auto result = i.runtime.process({std::uint32_t(frames), {main.data(), i.channels}, {out.data(), i.channels},
                plugin::descriptor().supportsSidechain ? std::span<const float* const>(side) : std::span<const float* const>{},
                {}, {i.midi.data(), i.midiCount}, i.transport()});
            (void)result; // Runtime silences admitted output buffers on failure.
            ParameterEdit edit; while (i.runtime.engine().pollGenerated(edit)) i.sendEdit(edit);
        }
        i.midiCount = 0; i.midiOverflow = false;
    }
    bool receiveEvents(const VstEvents* events) noexcept {
        if (!plugin::descriptor().acceptsMidi) return false;
        if (!events || events->numEvents < 0 || events->numEvents > 65536) { midiOverflow = true; return false; }
        for (VstInt32 index = 0; index < events->numEvents; ++index) {
            const auto* event = events->events[index];
            if (!event || event->type != kVstMidiType || event->byteSize < VstInt32(sizeof(VstMidiEvent))) continue;
            const auto* input = reinterpret_cast<const VstMidiEvent*>(event);
            const auto status = std::uint8_t(input->midiData[0]);
            if (input->deltaFrames < 0 || status < 0x80 || status >= 0xf0) continue;
            if (midiCount == midi.size()) { midiOverflow = true; return false; }
            const auto size = std::uint8_t((status & 0xf0) == 0xc0 || (status & 0xf0) == 0xd0 ? 2 : 3);
            midi[midiCount++] = {std::uint32_t(input->deltaFrames),
                {status, std::uint8_t(input->midiData[1]), size == 3 ? std::uint8_t(input->midiData[2]) : std::uint8_t(0)}, size};
        }
        return !midiOverflow;
    }

#if CELL_HAS_UI
    bool ensureEditor() {
        if (editor) return true;
        auto candidate = plugin::createEditor(runtime.engine()); if (!candidate) return false;
        candidate->editsAvailable = [this] { uiEdits(); };
        candidate->transientInput=[this](std::span<const std::byte> bytes){return !bytes.empty()&&bytes.size()<=maxProductQueryBytes&&runtime.engine().executeProductCommand(bytes);};
        candidate->productCommand = [this](std::span<const std::byte> bytes) {
            auto* target = dynamic_cast<CommandTarget*>(&runtime.engine().processor());
            if (!target || bytes.size() > 16384 || !target->executeCommand(bytes)) return false;
            call(audioMasterUpdateDisplay); return true;
        };
        candidate->productQuery = [this](std::span<const std::byte> bytes, bool) {
            if (bytes.empty() || bytes.size() > maxProductQueryBytes) return std::vector<std::byte>{};
            auto reply = runtime.engine().queryProduct(bytes); return reply.size() <= maxProductReplyBytes ? reply : std::vector<std::byte>{};
        };
        candidate->restoreProductState = [this](std::span<const std::byte> bytes) {
            if (runtime.loadState(bytes) != Result::ok) return false;
            refreshLatency(); call(audioMasterUpdateDisplay); return true;
        };
        candidate->requestSize = [this](int width, int height) {
            if (width < 1 || height < 1 || width > 9600 || height > 7200 || !call(audioMasterSizeWindow, width, height)) return false;
            if (window && window->isOpen() && !window->resize(width, height)) return false;
            rectangle.right = VstInt16(width); rectangle.bottom = VstInt16(height); return true;
        };
        const auto size = candidate->preferredSize();
        if (!std::isfinite(size.x) || !std::isfinite(size.y) || size.x < 1 || size.y < 1 || size.x > 9600 || size.y > 7200) return false;
        rectangle = {0, 0, VstInt16(size.y), VstInt16(size.x)};
        auto candidateWindow = std::make_unique<ui::Window>(*candidate);
        editor = std::move(candidate); window = std::move(candidateWindow);
        return true;
    }
#endif

    VstIntPtr dispatcher(VstInt32 opcode, VstInt32 index, VstIntPtr value, void* pointer, float option) {
        const auto* field = parameter(index);
        switch (opcode) {
        case effOpen: call(audioMasterNeedIdle); return 1;
        case effClose:
#if CELL_HAS_UI
            if (window) { window->show(false); window->close(); } uiEdits();
#endif
            delete this; return 1;
        case effGetVstVersion: return 2400;
        case effGetPlugCategory: return kPlugCategEffect;
        case effGetEffectName: copy(static_cast<char*>(pointer), kVstMaxEffectNameLen, plugin::descriptor().name); return 1;
        case effGetProductString: copy(static_cast<char*>(pointer), kVstMaxProductStrLen, plugin::descriptor().name); return 1;
        case effGetVendorString: copy(static_cast<char*>(pointer), kVstMaxVendorStrLen, plugin::descriptor().vendor); return 1;
        case effGetVendorVersion: return CELL_PLUGIN_VERSION_CODE;
        case effGetProgram: return 0;
        case effSetProgram: return value == 0;
        case effGetProgramName: copy(static_cast<char*>(pointer), kVstMaxProgNameLen, "Current"); return 1;
        case effGetProgramNameIndexed: if (index) return 0; copy(static_cast<char*>(pointer), kVstMaxProgNameLen, "Current"); return 1;
        case effSetSampleRate:
            if (runtime.phase() == Phase::processing || runtime.phase() == Phase::active || !std::isfinite(option) || option < 1000 || option > 768000) return 0;
            sampleRate = option; return 1;
        case effSetBlockSize:
            if (runtime.phase() == Phase::processing || runtime.phase() == Phase::active || value < 1 || value > wrapper::Limits::hostFrames) return 0;
            blockSize = std::uint32_t(value); return 1;
        case effMainsChanged: return mains(value != 0);
        case effStartProcess: return runtime.phase() == Phase::processing || runtime.start() == Result::ok;
        case effStopProcess: return runtime.phase() == Phase::active || runtime.stop() == Result::ok;
        case effGetParamName: if (!field) return 0; copy(static_cast<char*>(pointer), kVstMaxParamStrLen, field->name.c_str()); return 1;
        case effGetParamLabel: if (!field) return 0; copy(static_cast<char*>(pointer), kVstMaxParamStrLen, field->unit.c_str()); return 1;
        case effGetParamDisplay: {
            if (!field || !pointer) return 0;
            char text[64]; std::snprintf(text, sizeof(text), field->step >= 1 ? "%.0f" : "%.2f", runtime.engine().parameters().get(field->id));
            copy(static_cast<char*>(pointer), kVstMaxParamStrLen, text); return 1;
        }
        case effCanBeAutomated: return field && field->automatable;
        case effString2Parameter: {
            if (!field || !pointer) return 0;
            const auto* text = static_cast<const char*>(pointer); const auto length = std::strlen(text); if (length > 128) return 0;
            double parsed{}; const auto result = cell::detail::parseDoubleChars(text, text + length, parsed);
            if (result.ec != std::errc{} || !std::isfinite(parsed)) return 0;
            std::string_view suffix(result.ptr, std::size_t(text + length - result.ptr));
            while (!suffix.empty() && suffix.front() == ' ') suffix.remove_prefix(1);
            while (!suffix.empty() && suffix.back() == ' ') suffix.remove_suffix(1);
            if (!suffix.empty() && suffix != field->unit) return 0;
            return runtime.engine().parameters().set(field->id, parsed);
        }
        case effGetChunk: {
            if (!pointer) return 0;
            std::lock_guard lock(chunkMutex);
            std::vector<std::byte> candidate;
            if (runtime.saveState(candidate) != Result::ok) { *static_cast<void**>(pointer) = nullptr; return 0; }
            chunk = std::move(candidate); *static_cast<void**>(pointer) = chunk.data(); return VstIntPtr(chunk.size());
        }
        case effSetChunk:
            if (!pointer || value <= 0 || std::uint64_t(value) > wrapper::Limits::stateBytes ||
                runtime.loadState({static_cast<const std::byte*>(pointer), std::size_t(value)}) != Result::ok) return 0;
            refreshLatency(); call(audioMasterUpdateDisplay); return 1;
        case effProcessEvents: return receiveEvents(static_cast<const VstEvents*>(pointer));
        case effGetNumMidiInputChannels: return plugin::descriptor().acceptsMidi ? 16 : 0;
        case effGetNumMidiOutputChannels: return 0;
        case effGetTailSize: {
            const auto tail=runtime.engine().processor().tail();
            return tail?VstIntPtr(std::min(tail,std::uint32_t(std::numeric_limits<VstInt32>::max()))):1;
        }
        case effSetSpeakerArrangement: {
            if (runtime.phase() == Phase::active || runtime.phase() == Phase::processing) return 0;
            const auto* in = reinterpret_cast<const VstSpeakerArrangement*>(value);
            const auto* out = static_cast<const VstSpeakerArrangement*>(pointer);
            if (!in || !out || in->numChannels != out->numChannels ||
                (out->numChannels != VstInt32(plugin::descriptor().channels) && !(plugin::descriptor().supportsMono && out->numChannels == 1))) return 0;
            channels = std::uint32_t(out->numChannels);
            inputArrangement.type = outputArrangement.type = channels == 1 ? kSpeakerArrMono : kSpeakerArrStereo;
            inputArrangement.numChannels = outputArrangement.numChannels = VstInt32(channels);
            effect.numInputs = VstInt32(channels + (plugin::descriptor().supportsSidechain ? 2 : 0)); effect.numOutputs = VstInt32(channels); return 1;
        }
        case effGetSpeakerArrangement:
            if (!value || !pointer) return 0;
            *reinterpret_cast<VstSpeakerArrangement**>(value) = &inputArrangement;
            *static_cast<VstSpeakerArrangement**>(pointer) = &outputArrangement; return 1;
        case effGetInputProperties: case effGetOutputProperties: {
            const bool input = opcode == effGetInputProperties;
            if (!pointer || index < 0 || index >= (input ? effect.numInputs : effect.numOutputs)) return 0;
            auto& pin = *static_cast<VstPinProperties*>(pointer); pin = {};
            const bool side = input && std::uint32_t(index) >= channels;
            copy(pin.label, sizeof(pin.label), side ? "Sidechain" : input ? "Input" : "Output");
            copy(pin.shortLabel, sizeof(pin.shortLabel), side ? "SC" : input ? "In" : "Out");
            pin.flags = kVstPinIsActive; if ((side || channels == 2) && (index % 2 == (side ? int(channels % 2) : 0))) pin.flags |= kVstPinIsStereo;
            return 1;
        }
        case effCanDo:
            if (!pointer) return 0;
            if (!std::strcmp(static_cast<const char*>(pointer), "receiveVstTimeInfo")) return 1;
            if (!std::strcmp(static_cast<const char*>(pointer), "receiveVstEvents") || !std::strcmp(static_cast<const char*>(pointer), "receiveVstMidiEvent")) return plugin::descriptor().acceptsMidi ? 1 : -1;
            return 0;
        case effIdle: refreshLatency(); return 1;
#if CELL_HAS_UI
        case effEditGetRect:
            if (!pointer || !ensureEditor()) return 0; *static_cast<ERect**>(pointer) = &rectangle; return 1;
        case effEditOpen:
            if (!pointer || !ensureEditor() || window->isOpen()) return 0;
            if (!window->open(reinterpret_cast<std::uintptr_t>(pointer), rectangle.right, rectangle.bottom)) return 0;
            window->show(true); return 1;
        case effEditClose:
            if (window) { window->show(false); window->close(); } uiEdits(); window.reset(); editor.reset(); return 1;
        case effEditIdle:
            if (window && window->isOpen()) window->poll(); uiEdits(); refreshLatency(); return 1;
#endif
        default: return 0;
        }
    }
    static VstIntPtr VSTCALLBACK dispatch(AEffect* effect, VstInt32 opcode, VstInt32 index, VstIntPtr value, void* pointer, float option) noexcept {
        try { if (auto* instance = get(effect)) return instance->dispatcher(opcode, index, value, pointer, option); }
        catch (...) {} // Never unwind through the host ABI.
        return 0;
    }
};
}

#if defined(_WIN32)
#define CELL_VST2_EXPORT __declspec(dllexport)
#else
#define CELL_VST2_EXPORT __attribute__((visibility("default")))
#endif
extern "C" CELL_VST2_EXPORT AEffect* VSTCALLBACK VSTPluginMain(audioMasterCallback host) noexcept {
    if (!host || host(nullptr, audioMasterVersion, 0, 0, nullptr, 0) < 2400) return nullptr;
    try {
        if (cell::plugin::descriptor().channels == 0) return nullptr;
        return &(new Instance(host))->effect;
    } catch (...) { return nullptr; }
}
#if defined(__APPLE__)
extern "C" CELL_VST2_EXPORT AEffect* VSTCALLBACK main_macho(audioMasterCallback host) noexcept { return VSTPluginMain(host); }
#endif
