#if defined(CELL_VST3_WEB_EDITOR) && defined(_WIN32)
#include "webview2_plugin.hpp"
#elif defined(CELL_VST3_WEB_EDITOR) && defined(__APPLE__)
#include "cocoa_web_editor.hpp"
#endif
#include <cell/jobs.hpp>
#include <cell/midi_delivery.hpp>
#include <cell/commands.hpp>
#include <cell/operation_ack.hpp>
#include <cell/plugin.hpp>
#include <cell/analysis.hpp>
#include <cell/callback_gate.hpp>
#if CELL_HAS_UI
#include <cell/ui.hpp>
#include <cell/ui_scaling.hpp>
#include "pluginterfaces/gui/iplugviewcontentscalesupport.h"
#include "pluginterfaces/base/keycodes.h"
#endif
#if CELL_HAS_UI || defined(CELL_VST3_WEB_EDITOR)
#include "public.sdk/source/common/pluginview.h"
#endif
#if defined(CELL_VST3_WEB_EDITOR)
#include <cell/webview.hpp>
#include <cell/web_meter.hpp>
#include "cell_vst3_web_bindings.hpp"
#include "cell_vst3_web_page.hpp"
#endif
#include "public.sdk/source/main/pluginfactory.h"
#include "public.sdk/source/vst/vstaudioeffect.h"
#include "public.sdk/source/vst/vsteditcontroller.h"
#include "pluginterfaces/base/ibstream.h"
#include "pluginterfaces/base/ustring.h"
#include "pluginterfaces/vst/ivstparameterchanges.h"
#include "pluginterfaces/vst/ivstprocesscontext.h"
#include <cell/reaper_project.hpp>
#include "pluginterfaces/vst/ivstcontextmenu.h"
#include "pluginterfaces/vst/ivstdataexchange.h"
#include "pluginterfaces/vst/ivstmessage.h"
#include "pluginterfaces/vst/ivstevents.h"
#include "pluginterfaces/vst/ivstmidicontrollers.h"
#include <cstring>
#include <chrono>

namespace {
using namespace Steinberg;
using namespace Steinberg::Vst;
#ifndef CELL_VST3_PROCESSOR_ID
#define CELL_VST3_PROCESSOR_ID 0x915DC720,0x4461473B,0x9C55C8FE,0xF1692411
#define CELL_VST3_CONTROLLER_ID 0x27BCD148,0x428D4118,0xBE367455,0x67D8BA9C
#endif
#ifndef CELL_VST3_CONTROLLER_NAME
#define CELL_VST3_CONTROLLER_NAME "Cell Probe Controller"
#endif
#ifndef CELL_VST3_SUBCATEGORIES
#define CELL_VST3_SUBCATEGORIES "Fx|Distortion"
#endif
#ifndef CELL_VST3_CLASS_FLAGS
#define CELL_VST3_CLASS_FLAGS Vst::kDistributable
#endif
const FUID processorId(CELL_VST3_PROCESSOR_ID);
const FUID controllerId(CELL_VST3_CONTROLLER_ID);

#if defined(CELL_VST3_WEB_EDITOR)
constexpr auto designerMeterRequest = "Cell.Designer.Meter.Request.v1";
constexpr auto designerMeterReadings = "Cell.Designer.Meter.Readings.v1";
constexpr auto designerCommandRequest = "Cell.Designer.Command.Request.v1";
constexpr auto designerCommandReply = "Cell.Designer.Command.Reply.v1";
enum class DesignerOperation : int64 { poll, apply, cancel, close };
struct DesignerEdit {
    std::uint32_t id, type;
    double value;
};
static_assert(sizeof(DesignerEdit) == 16);
constexpr std::size_t designerCommandBytes = 1024, designerSnapshotBytes = 65536, designerMaxEdits = 2048;
struct DesignerReply {
    std::string snapshot;
    std::vector<DesignerEdit> edits;
    bool changed{};
};
constexpr bool designerHasOutputMeter = std::ranges::any_of(cell::designer::meters, [](std::string_view key) {
    return key == "output-peak" || key == "output-rms";
});
#endif

SpeakerArrangement arrangement(cell::AudioLayout layout)noexcept{
    switch(layout){case cell::AudioLayout::mono:return SpeakerArr::kMono;case cell::AudioLayout::stereo:return SpeakerArr::kStereo;case cell::AudioLayout::surround51:return SpeakerArr::k51;case cell::AudioLayout::surround712:return SpeakerArr::k71_2;case cell::AudioLayout::surround714:return SpeakerArr::k71_4;case cell::AudioLayout::surround916:return SpeakerArr::k91_6_W;}return SpeakerArr::kEmpty;
}

bool readState(IBStream* stream,cell::Engine& engine) {
    if(!stream)return false;
    std::vector<std::byte> bytes;std::array<std::byte,4096> scratch;
    for(;;){int32 count{};const auto result=stream->read(scratch.data(),int32(scratch.size()),&count);
        if(count<0||count>int32(scratch.size()))return false;
        if(count==0){if(result!=kResultOk&&result!=kResultFalse)return false;break;}
        if(bytes.size()+std::size_t(count)>128*1024*1024)return false;
        bytes.insert(bytes.end(),scratch.begin(),scratch.begin()+count);
    }
    return engine.loadState(bytes);
}
bool writeState(IBStream* stream,const cell::Engine& engine) {
    if(!stream)return false;
    const auto bytes=engine.saveState();std::size_t offset{};
    while(offset<bytes.size()){int32 count{};stream->write(const_cast<std::byte*>(bytes.data()+offset),int32(bytes.size()-offset),&count);
        if(count<=0||std::size_t(count)>bytes.size()-offset)return false;
        offset+=std::size_t(count);}
    return true;
}

constexpr double midi2Maximum = 4294967295.0;
constexpr NoteExpressionTypeID assignableStart = 201000, registeredStart = 202000;
constexpr int controllersPerKind = 128;

cell::MidiEvent notePacket(std::uint32_t frame, unsigned channel, unsigned note, int32 noteId,
                            unsigned status, float velocity) noexcept {
    const std::array words{
        std::uint32_t(0x40000000u | (status << 20) | (channel << 16) | (note << 8)),
        static_cast<std::uint32_t>(std::lround(std::clamp(velocity, 0.f, 1.f) * 65535.f)) << 16};
    auto result = cell::MidiEvent::universalPacket(frame, words);
    result.noteId = noteId;
    return result;
}

cell::MidiEvent expressionPacket(std::uint32_t frame, unsigned channel, unsigned note, int32 noteId,
                                  unsigned status, unsigned controller, double value) noexcept {
    const std::array words{
        std::uint32_t(0x40000000u | (status << 20) | (channel << 16) | (note << 8) | controller),
        static_cast<std::uint32_t>(std::round(std::clamp(value, 0.0, 1.0) * midi2Maximum))};
    auto result = cell::MidiEvent::universalPacket(frame, words);
    result.noteId = noteId;
    return result;
}

class InputNotes {
public:
    struct Voice { int32 id{-1}; int16 channel{}, pitch{}; };

    const Voice* find(int32 id) const noexcept {
        if (id == -1) return nullptr;
        const auto entry = std::ranges::find(voices_, id, &Voice::id);
        return entry == voices_.end() ? nullptr : &*entry;
    }
    void on(int32 id, int16 channel, int16 pitch) noexcept {
        if (id == -1) return;
        auto entry = std::ranges::find(voices_, id, &Voice::id);
        if (entry == voices_.end()) entry = std::ranges::find(voices_, -1, &Voice::id);
        if (entry != voices_.end()) *entry = {id, channel, pitch};
    }
    void off(int32 id, int16 channel, int16 pitch) noexcept {
        // An ID-less release cannot identify which overlapping instance ended.
        for (auto& voice : voices_)
            if ((id != -1 && voice.id == id) || (id == -1 && voice.channel == channel && voice.pitch == pitch))
                voice.id = -1;
    }
    void clear() noexcept { voices_.fill({}); }
private:
    std::array<Voice, 256> voices_{};
};

std::optional<cell::MidiEvent> noteExpression(const InputNotes& notes, std::uint32_t frame,
                                               const NoteExpressionValueEvent& event) noexcept {
    const auto* note = notes.find(event.noteId);
    if (!note || !std::isfinite(event.value) || event.value < 0 || event.value > 1) return {};
    const auto type = event.typeId;
    unsigned status{}, controller{};
    if (type >= registeredStart && type < registeredStart + controllersPerKind) {
        controller = type - registeredStart;
    } else if (type >= assignableStart && type < assignableStart + controllersPerKind) {
        status = 1;
        controller = type - assignableStart;
    } else if (type == kTuningTypeID) {
        status = 6;
    } else return {};
    return expressionPacket(frame, note->channel, note->pitch, note->id, status, controller, event.value);
}

// VST3 event lists may copy the DataEvent pointer without copying its bytes.
struct MidiOutput final:cell::midi::Output {
    IEventList* destination{};
    cell::midi::SysExStorage* sysEx{};
    bool supportsUmp{};
    bool push(cell::MidiEvent message)noexcept override{
        if (message.isUmp()) return supportsUmp && destination && pushUmp(message);
        if (message.isSysEx()) {
            if (!destination || !sysEx) return false;
            const auto bytes = sysEx->copy(message.sysEx);
            if (bytes.empty()) return false;
            Event event{};
            event.busIndex = 0;
            event.sampleOffset = int32(message.frame);
            event.type = Event::kDataEvent;
            event.data = {uint32(bytes.size()), DataEvent::kMidiSysEx, bytes.data()};
            return destination->addEvent(event) == kResultOk;
        }
        if(!destination||!cell::midi::valid(message))return false;
        Event e{};e.busIndex=0;e.sampleOffset=int32(message.frame);const auto kind=message.data[0]&0xf0;const auto channel=message.data[0]&15;
        if(kind==0x90&&message.data[2]){e.type=Event::kNoteOnEvent;e.noteOn.channel=int16(channel);e.noteOn.pitch=message.data[1];e.noteOn.velocity=message.data[2]/127.f;e.noteOn.noteId=message.noteId;}
        else if(kind==0x80||kind==0x90){e.type=Event::kNoteOffEvent;e.noteOff.channel=int16(channel);e.noteOff.pitch=message.data[1];e.noteOff.velocity=message.data[2]/127.f;e.noteOff.noteId=message.noteId;}
        else if(kind==0xa0){e.type=Event::kPolyPressureEvent;e.polyPressure.channel=int16(channel);e.polyPressure.pitch=message.data[1];e.polyPressure.pressure=message.data[2]/127.f;e.polyPressure.noteId=message.noteId;}
        else {e.type=Event::kLegacyMIDICCOutEvent;e.midiCCOut.channel=int8(channel);
            if(kind==0xb0){e.midiCCOut.controlNumber=message.data[1];e.midiCCOut.value=message.data[2];}
            else if(kind==0xc0){e.midiCCOut.controlNumber=kCtrlProgramChange;e.midiCCOut.value=message.data[1];}
            else if(kind==0xd0){e.midiCCOut.controlNumber=kAfterTouch;e.midiCCOut.value=message.data[1];}
            else if(kind==0xe0){e.midiCCOut.controlNumber=kPitchBend;e.midiCCOut.value=message.data[1];e.midiCCOut.value2=message.data[2];}
            else return false;
        }
        return destination->addEvent(e)==kResultOk;
    }

    bool pushUmp(cell::MidiEvent message) noexcept {
        if (!cell::midi::validUmp(message)) return false;
        const auto word = message.ump[0], type = word >> 28;
        // VST3's event bus carries one 16-channel MIDI group.
        if ((word >> 24) & 15) return false;
        const auto status = static_cast<std::uint8_t>(word >> 16);
        const int16 channel = status & 15;
        const int16 pitch = (word >> 8) & 255;
        if (pitch > 127) return false;
        const auto noteId = message.noteId != -1 ? message.noteId :
            kNoteIDUserRangeLowerBound + channel * 128 + pitch;
        if (type == 2) {
            return push({message.frame, {status, static_cast<std::uint8_t>(word >> 8), static_cast<std::uint8_t>(word)},
                static_cast<std::uint8_t>((status & 0xe0) == 0xc0 ? 2 : 3)});
        }
        if (type != 4) return false;
        Event output{};
        output.sampleOffset = int32(message.frame);
        const auto kind = status >> 4;
        const auto velocity = float(message.ump[1] >> 16) / 65535.f;
        if (kind == 8) {
            output.type = Event::kNoteOffEvent;
            output.noteOff = {channel, pitch, velocity, noteId, 0};
        } else if (kind == 9) {
            output.type = Event::kNoteOnEvent;
            output.noteOn = {channel, pitch, 0, velocity, 0, noteId};
        } else if (kind == 0xb) {
            const auto scaled = static_cast<std::uint8_t>(std::round(message.ump[1] / midi2Maximum * 127.0));
            return push({message.frame, {status, static_cast<std::uint8_t>(pitch), scaled}});
        } else {
            const auto expression = cell::midi::perNoteExpression(message);
            if (!expression) return false;
            const auto normalized = expression->value / midi2Maximum;
            using Kind = cell::midi::PerNoteExpression::Kind;
            if (expression->kind == Kind::pressure) {
                output.type = Event::kPolyPressureEvent;
                output.polyPressure = {channel, pitch, static_cast<float>(normalized), noteId};
            } else {
                output.type = Event::kNoteExpressionValueEvent;
                const auto id = expression->kind == Kind::registeredController ? registeredStart + expression->controller :
                    expression->kind == Kind::assignableController ? assignableStart + expression->controller : kTuningTypeID;
                output.noteExpressionValue = {id, noteId, normalized};
            }
        }
        return destination->addEvent(output) == kResultOk;
    }
};

class Processor final : public AudioEffect {
public:
    Processor(){setControllerClass(controllerId);analysis_=dynamic_cast<cell::AnalysisSource*>(&engine_.processor());}
    static FUnknown* create(void*){
        return cell::plugin::descriptor().channels ? static_cast<IAudioProcessor*>(new Processor) : nullptr;
    }
    tresult PLUGIN_API initialize(FUnknown* context) override {
        const auto result=AudioEffect::initialize(context);if(result!=kResultOk)return result;
        if(auto* extension=dynamic_cast<cell::NativeHostContext*>(&engine_.processor()))extension->nativeHostContext("VST3",context,true);
        if(auto* extension=dynamic_cast<cell::HostProjectContext*>(&engine_.processor()))if(!extension->hostProjectContext(cell::vst3::projectIdentity(context)))return kResultFalse;
        addAudioInput(STR16("Input"),SpeakerArr::kStereo);addAudioOutput(STR16("Output"),SpeakerArr::kStereo);if(cell::plugin::descriptor().supportsSidechain)addAudioInput(STR16("Sidechain"),SpeakerArr::kStereo,BusTypes::kAux,0);if(cell::plugin::descriptor().acceptsMidi)addEventInput(STR16("MIDI Input"),16);if(cell::plugin::descriptor().producesMidi)addEventOutput(STR16("MIDI Output"),16);return kResultOk;
    }
    tresult PLUGIN_API setBusArrangements(SpeakerArrangement* in,int32 inputs,SpeakerArrangement* out,int32 outputs) override {
        const auto& descriptor=cell::plugin::descriptor();
        if(active_||(inputs!=1&&!(descriptor.supportsSidechain&&inputs==2))||outputs!=1||!in||!out||
            (inputs==2&&in[1]!=SpeakerArr::kMono&&in[1]!=SpeakerArr::kStereo))return kResultFalse;
        if(descriptor.mainBusLayouts.empty()){
            if(in[0]!=out[0]||(in[0]!=SpeakerArr::kStereo&&!(descriptor.supportsMono&&in[0]==SpeakerArr::kMono)))return kResultFalse;
        }else if(std::none_of(descriptor.mainBusLayouts.begin(),descriptor.mainBusLayouts.end(),[&](const auto& layout){return in[0]==arrangement(layout.input)&&out[0]==arrangement(layout.output);}))return kResultFalse;
        const auto result=AudioEffect::setBusArrangements(in,inputs,out,outputs);
        if(result==kResultOk){channels_=SpeakerArr::getChannelCount(out[0]);inputChannels_=SpeakerArr::getChannelCount(in[0]);}
        return result;
    }
    tresult PLUGIN_API canProcessSampleSize(int32 size) override {return size==kSample32?kResultTrue:kResultFalse;}
    tresult PLUGIN_API setupProcessing(ProcessSetup& setup) override {
        if(active_||setup.symbolicSampleSize!=kSample32||setup.maxSamplesPerBlock<=0)return kInvalidArgument;
        try {
            engine_.prepare(setup.sampleRate, std::uint32_t(setup.maxSamplesPerBlock), channels_);
            if (engine_.descriptor().supportsSysEx && engine_.descriptor().producesMidi)
                sysExOutput_.prepare(cell::midi::sysExByteCapacity);
        } catch (...) { return kInvalidArgument; }
        const auto result=AudioEffect::setupProcessing(setup);
        if(result==kResultOk){reportedLatency_=engine_.processor().latency();sampleRate_=setup.sampleRate;maximumFrames_=std::uint32_t(setup.maxSamplesPerBlock);setupValid_=activationPrepared_=true;openAnalysisQueue();}return result;
    }
    tresult PLUGIN_API setActive(TBool active) override {
        if(active){
            if(!setupValid_)return kResultFalse;
            // Hosts can deactivate/reactivate without calling setupProcessing
            // again. Rebuild stopped resources so old filter and delay history
            // cannot leak into a new render. Activation is a control operation.
            if(!activationPrepared_)try{engine_.prepare(sampleRate_,maximumFrames_,channels_);}catch(...){return kResultFalse;}
            engine_.reset(); inputNotes_.clear(); openAnalysisQueue();
        }
        const auto result=AudioEffect::setActive(active);
        if(result==kResultOk){active_=active;if(active)activationPrepared_=false;}
        if(!active&&result==kResultOk){midiDelivery_.reset();closeAnalysisQueue();}
        return result;
    }
    tresult PLUGIN_API connect(IConnectionPoint* other) override {
        const auto result=AudioEffect::connect(other);if(result==kResultOk){connected_=true;openAnalysisQueue();}return result;
    }
    tresult PLUGIN_API disconnect(IConnectionPoint* other) override {
        if(!active_)closeAnalysisQueue();
#if defined(CELL_VST3_WEB_EDITOR)
        editorMeter_.setEnabled(false);
        meterSession_ = 0;
        meterClosed_ = false;
        designerCommands_.cancel();
        commandSession_ = 0;
        commandClosed_ = false;
#endif
        connected_=false;return AudioEffect::disconnect(other);
    }
    tresult PLUGIN_API terminate() override {if(!active_)closeAnalysisQueue();if(auto* extension=dynamic_cast<cell::NativeHostContext*>(&engine_.processor()))extension->nativeHostContext("VST3",nullptr,true);return AudioEffect::terminate();}
    tresult PLUGIN_API notify(IMessage* message) override {
#if defined(CELL_VST3_WEB_EDITOR)
        if (message && message->getMessageID() && !std::strcmp(message->getMessageID(), designerCommandRequest)) {
            try { return handleDesignerCommand(message); }
            catch (...) { return kResultFalse; }
        }
        if (message && message->getMessageID() && !std::strcmp(message->getMessageID(), designerMeterRequest)) {
            auto* attributes = message->getAttributes();
            int64 session{}, visible{};
            if (!attributes || attributes->getInt("session", session) != kResultOk || session <= 0 ||
                attributes->getInt("visible", visible) != kResultOk || (visible != 0 && visible != 1)) return kInvalidArgument;
            if (session < meterSession_ || (session == meterSession_ && meterClosed_)) return kResultOk;
            meterSession_ = session;
            meterClosed_ = !visible;
            editorMeter_.setEnabled(visible != 0);
            if (!visible) return kResultOk;
            auto reply = owned(allocateMessage());
            if (!reply) return kResultFalse;
            reply->setMessageID(designerMeterReadings);
            const auto levels = editorMeter_.levels();
            auto* fields = reply->getAttributes();
            if (!fields || fields->setInt("session", session) != kResultOk ||
                fields->setFloat("peak", levels.peak) != kResultOk || fields->setFloat("rms", levels.rms) != kResultOk)
                return kResultFalse;
            return sendMessage(reply);
        }
#endif
        if(message&&message->getMessageID()&&!std::strcmp(message->getMessageID(),cell::productQueryMessage)){
            const void* bytes{};uint32 size{};int64 token{};auto* attributes=message->getAttributes();
            if(!attributes||attributes->getInt("token",token)!=kResultOk||token<=0||attributes->getBinary("query",bytes,size)!=kResultOk||!bytes||!size||size>cell::maxProductQueryBytes)return kResultFalse;
            try{auto data=engine_.queryProduct({static_cast<const std::byte*>(bytes),size});if(data.size()>cell::maxProductReplyBytes)return kResultFalse;auto reply=owned(allocateMessage());if(!reply)return kResultFalse;reply->setMessageID(cell::productQueryReplyMessage);reply->getAttributes()->setInt("token",token);reply->getAttributes()->setInt("size",int64(data.size()));if(!data.empty())reply->getAttributes()->setBinary("data",data.data(),uint32(data.size()));return sendMessage(reply);}catch(...){return kResultFalse;}
        }
        if(message&&message->getMessageID()&&(!std::strcmp(message->getMessageID(),cell::productCommandMessage)||!std::strcmp(message->getMessageID(),cell::productInputMessage))){const void* data{};uint32 size{};auto* attributes=message->getAttributes();auto* target=dynamic_cast<cell::CommandTarget*>(&engine_.processor());if(!target||!attributes||attributes->getBinary("command",data,size)!=kResultOk||!data||!size||size>16384)return kResultFalse;try{return target->executeCommand({static_cast<const std::byte*>(data),size})?kResultOk:kResultFalse;}catch(...){return kResultFalse;}}

        if(message&&message->getMessageID()&&!std::strcmp(message->getMessageID(),cell::productAcknowledgedCommandMessage)){
            const void* bytes{};uint32 size{};int64 token{};auto* attributes=message->getAttributes();
            if(!attributes||attributes->getInt("token",token)!=kResultOk||token<=0||attributes->getBinary("command",bytes,size)!=kResultOk||!bytes||!size||size>cell::maxProductQueryBytes)return kInvalidArgument;
            bool accepted=false;try{accepted=engine_.executeProductCommand({static_cast<const std::byte*>(bytes),size});}catch(...){accepted=false;}
            auto reply=owned(allocateMessage());if(!reply)return kResultFalse;
            reply->setMessageID(cell::productCommandReplyMessage);reply->getAttributes()->setInt("token",token);reply->getAttributes()->setInt("accepted",accepted?1:0);return sendMessage(reply);
        }
        if(message&&message->getMessageID()&&!std::strcmp(message->getMessageID(),cell::productRestoreMessage)){
            const void* bytes{};uint32 size{};int64 token{};auto* attributes=message->getAttributes();
            if(!attributes||attributes->getInt("token",token)!=kResultOk||token<=0||attributes->getBinary("state",bytes,size)!=kResultOk||!bytes||!size||size>128u*1024*1024)return kInvalidArgument;
            bool restored=false;
            try{restored=loadProductBytes({static_cast<const std::byte*>(bytes),size});}catch(...){restored=false;}
            auto reply=owned(allocateMessage());if(!reply)return kResultFalse;
            reply->setMessageID(cell::productRestoreReplyMessage);reply->getAttributes()->setInt("token",token);reply->getAttributes()->setInt("restored",restored?1:0);
            return sendMessage(reply);
        }
        if(message&&message->getMessageID()&&!std::strcmp(message->getMessageID(),"Cell.State.Restore.v1")){
            const void* bytes{};uint32 size{};auto* attributes=message->getAttributes();
            if(!attributes||attributes->getBinary("state",bytes,size)!=kResultOk||!bytes||size>128*1024*1024)return kResultFalse;
            try{return loadProductBytes({static_cast<const std::byte*>(bytes),size})?kResultOk:kResultFalse;}catch(...){return kResultFalse;}
        }
        if(message&&message->getMessageID()&&!std::strcmp(message->getMessageID(),cell::analysisEnableMessage)){
            int64 enabled{};auto* attributes=message->getAttributes();
            if(!attributes||attributes->getInt("enabled",enabled)!=kResultOk)return kInvalidArgument;
            analysisRequested_=enabled!=0;updateAnalysisState();return kResultOk;
        }
        if(message&&message->getMessageID()&&!std::strcmp(message->getMessageID(),"Cell.Analysis.Poll.v4")){
            // Main-thread IMessage fallback for hosts without IDataExchangeHandler.
            // The processor mailbox stays SPSC: audio produces, GUI consumes.
            if(exchange_||queue_!=InvalidDataExchangeQueueID||!setupValid_||!connected_||!analysis_)return kResultFalse;
            cell::AnalysisFrame latest;bool received=false;for(unsigned n=0;n<3;++n){cell::AnalysisFrame next;if(!analysis_->pullAnalysis(next))break;latest=next;received=true;}
            const auto latency=engine_.processor().latency();if(!received&&latency==reportedLatency_)return kResultOk;
            if(!received){latest.flags=1;latest.sampleRate=sampleRate_;}latest.latency=latency;
            if(auto reply=owned(allocateMessage())){reply->setMessageID("Cell.Analysis.Frame.v4");reply->getAttributes()->setBinary("frame",&latest,sizeof latest);const auto result=sendMessage(reply);if(result==kResultOk)reportedLatency_=latency;return result;}return kOutOfMemory;
        }
        return AudioEffect::notify(message);
    }
    tresult PLUGIN_API setProcessing(TBool active) override {
        if(!active){cell::CallbackGate::AudioScope callback(callbacks_);if(callback)engine_.reset();}
#if defined(CELL_VST3_WEB_EDITOR)
        if (!active) editorMeter_.clear();
#endif
        return kResultOk;
    }
    uint32 PLUGIN_API getLatencySamples() override {return engine_.processor().latency();}
    uint32 PLUGIN_API getTailSamples() override {return engine_.processor().tail();}
    tresult PLUGIN_API getState(IBStream* state) override {try{cell::CallbackGate::PauseScope pause(callbacks_,engine_.stateSaveRequiresStoppedProcessing());return pause&&writeState(state,engine_)?kResultOk:kResultFalse;}catch(...){return kResultFalse;}}
    tresult PLUGIN_API setState(IBStream* state) override {
        try{cell::CallbackGate::PauseScope pause(callbacks_,engine_.stateRequiresStoppedProcessing());return pause&&readState(state,engine_)?kResultOk:kResultFalse;}catch(...){return kResultFalse;}}
    tresult PLUGIN_API process(ProcessData& data) override {
        cell::CallbackGate::AudioScope callback(callbacks_);
        if(!callback){
            midiDelivery_.reset();
            if(data.symbolicSampleSize!=kSample32||data.numSamples<0)return kInvalidArgument;
            if(data.numSamples>0&&data.outputs)for(int32 b=0;b<data.numOutputs;++b){auto& bus=data.outputs[b];if(bus.channelBuffers32)for(int32 c=0;c<bus.numChannels;++c)if(bus.channelBuffers32[c])std::fill_n(bus.channelBuffers32[c],data.numSamples,0.f);bus.silenceFlags=~uint64(0);}
            return kResultOk;
        }
        auto& events=events_;std::size_t count{},midiCount{};
        if(data.inputParameterChanges)for(int32 q=0;q<data.inputParameterChanges->getParameterCount();++q) {
            auto* queue=data.inputParameterChanges->getParameterData(q);if(!queue)continue;
            const auto id=queue->getParameterId();
            if(cell::plugin::descriptor().acceptsMidi&&(id&0xfffff000u)==0x7f700000u){unsigned channel=(id>>8)&15,cc=id&255;if(cc>=130)continue;for(int32 point=0;point<queue->getPointCount();++point){int32 frame{};ParamValue value{};if(queue->getPoint(point,frame,value)!=kResultTrue||frame<0||frame>=data.numSamples||!std::isfinite(value))continue;if(midiCount==midi_.size())return kResultFalse;value=std::clamp(value,0.,1.);cell::MidiEvent event;event.frame=std::uint32_t(frame);if(cc<128)event.data={std::uint8_t(0xb0|channel),std::uint8_t(cc),std::uint8_t(std::lround(value*127))};else if(cc==128)event.data={std::uint8_t(0xd0|channel),std::uint8_t(std::lround(value*127)),0};else{unsigned wheel=unsigned(std::lround(value*16383));event.data={std::uint8_t(0xe0|channel),std::uint8_t(wheel&127),std::uint8_t(wheel>>7)};}midi_[midiCount++]=event;}continue;}
            const auto* param=engine_.parameters().find(id);if(!param)continue;
            for(int32 point=0;point<queue->getPointCount();++point){int32 frame{};ParamValue value{};
                if(queue->getPoint(point,frame,value)!=kResultTrue||frame<0||frame>data.numSamples||!std::isfinite(value))continue;
                if(count==events.size())return kResultFalse; // explicit failure, never silently lose automation
                events[count++]={std::uint32_t(frame),param->id,param->fromNormalized(value)};
            }
        }
        cell::stableEventSort({events.data(),count},eventScratch_);
        if(data.numSamples==0){for(std::size_t n=0;n<count;++n)engine_.parameters().set(events[n].id,events[n].value);return kResultOk;}
        if(data.numSamples<0||data.symbolicSampleSize!=kSample32||data.numOutputs!=1||!data.outputs||data.outputs[0].numChannels!=int32(channels_)||!data.outputs[0].channelBuffers32)return kResultFalse;
        auto** output=data.outputs[0].channelBuffers32;
        for(unsigned c=0;c<channels_;++c){if(!output[c])return kResultFalse;
            const float* input=data.numInputs>=1&&data.inputs&&c<inputChannels_&&data.inputs[0].numChannels==int32(inputChannels_)&&data.inputs[0].channelBuffers32?data.inputs[0].channelBuffers32[c]:nullptr;
            if(input)std::memmove(output[c],input,std::size_t(data.numSamples)*sizeof(float));else std::fill_n(output[c],data.numSamples,0.f);}
        cell::Transport transport;transport.offline=data.processMode==kOffline;transport.tempoValid=false;
        if(const auto* context=data.processContext){transport.playing=(context->state&ProcessContext::kPlaying)!=0;transport.recording=(context->state&ProcessContext::kRecording)!=0;transport.looping=(context->state&ProcessContext::kCycleActive)!=0;
            transport.samplePosition=context->projectTimeSamples;transport.samplePositionValid=true;if((context->state&ProcessContext::kTempoValid)&&std::isfinite(context->tempo)&&context->tempo>0){transport.tempo=context->tempo;transport.tempoValid=true;}
            if((context->state&ProcessContext::kProjectTimeMusicValid)&&std::isfinite(context->projectTimeMusic)){transport.beatPosition=context->projectTimeMusic;transport.beatPositionValid=true;}
            if((context->state&ProcessContext::kTimeSigValid)&&context->timeSigNumerator>0&&context->timeSigNumerator<=65535&&context->timeSigDenominator>0&&context->timeSigDenominator<=65535){transport.numerator=std::uint16_t(context->timeSigNumerator);transport.denominator=std::uint16_t(context->timeSigDenominator);}}
        cell::AudioBlock<float> side;if(cell::plugin::descriptor().supportsSidechain&&data.numInputs>1&&data.inputs&&data.inputs[1].channelBuffers32&&(data.inputs[1].numChannels==1||data.inputs[1].numChannels==2)){auto& bus=data.inputs[1];bool valid=true;for(int c=0;c<bus.numChannels;++c)valid&=bus.channelBuffers32[c]!=nullptr;if(valid)side={bus.channelBuffers32,std::uint32_t(bus.numChannels),std::uint32_t(data.numSamples),0};}
        cell::midi::SysExBudget sysEx;
        const auto& descriptor = engine_.descriptor();
        if (descriptor.acceptsMidi && data.inputEvents)
            for (int32 i = 0; i < data.inputEvents->getEventCount(); ++i) {
                Event event{};
                if (data.inputEvents->getEvent(i, event) != kResultOk || event.busIndex != 0 ||
                    event.sampleOffset < 0 || event.sampleOffset >= data.numSamples) continue;
                cell::MidiEvent midi;
                if (midiCount == midi_.size()) {
                    engine_.reset();
                    inputNotes_.clear();
                    midiDelivery_.reset();
                    return kResultFalse;
                }
                midi.frame = std::uint32_t(event.sampleOffset);
                if (event.type == Event::kNoteOnEvent) {
                    const auto& e = event.noteOn;
                    if (e.channel < 0 || e.channel > 15 || e.pitch < 0 || e.pitch > 127 || !std::isfinite(e.velocity)) continue;
                    midi.data = {std::uint8_t(0x90 | e.channel), std::uint8_t(e.pitch), std::uint8_t(std::lround(std::clamp(e.velocity, 0.f, 1.f) * 127))};
                    if (descriptor.supportsUmp) {
                        midi = notePacket(midi.frame, e.channel, e.pitch, e.noteId, 9, e.velocity);
                        inputNotes_.on(e.noteId, e.channel, e.pitch);
                    }
                } else if (event.type == Event::kNoteOffEvent) {
                    const auto& e = event.noteOff;
                    if (e.channel < 0 || e.channel > 15 || e.pitch < 0 || e.pitch > 127) continue;
                    midi.data = {std::uint8_t(0x80 | e.channel), std::uint8_t(e.pitch), 0};
                    if (descriptor.supportsUmp) {
                        midi = notePacket(midi.frame, e.channel, e.pitch, e.noteId, 8,
                                          std::isfinite(e.velocity) ? e.velocity : 0.f);
                        inputNotes_.off(e.noteId, e.channel, e.pitch);
                    }
                } else if (event.type == Event::kPolyPressureEvent) {
                    const auto& e = event.polyPressure;
                    if (e.channel < 0 || e.channel > 15 || e.pitch < 0 || e.pitch > 127 || !std::isfinite(e.pressure)) continue;
                    midi.data = {std::uint8_t(0xa0 | e.channel), std::uint8_t(e.pitch), std::uint8_t(std::lround(std::clamp(e.pressure, 0.f, 1.f) * 127))};
                    if (descriptor.supportsUmp)
                        midi = expressionPacket(midi.frame, e.channel, e.pitch, e.noteId, 0xa, 0, e.pressure);
                } else if (event.type == Event::kNoteExpressionValueEvent && descriptor.supportsUmp) {
                    const auto mapped = noteExpression(inputNotes_, midi.frame, event.noteExpressionValue);
                    if (!mapped) continue;
                    midi = *mapped;
                } else if (event.type == Event::kDataEvent && descriptor.supportsSysEx) {
                    const auto& e = event.data;
                    if (e.type != DataEvent::kMidiSysEx || !e.bytes || !sysEx.admit({e.bytes, e.size})) continue;
                    midi = cell::MidiEvent::systemExclusive(midi.frame, {e.bytes, e.size});
                } else continue;
                midi_[midiCount++] = midi;
            }
        cell::stableFrameSort<cell::MidiEvent>({midi_.data(),midiCount},midiScratch_);
        MidiOutput midiOutput;
        const auto* midiBus = getEventOutput(0);
        midiOutput.destination = midiBus && midiBus->isActive() ? data.outputEvents : nullptr;
        midiOutput.supportsUmp = descriptor.supportsUmp;
        const bool sends = descriptor.producesMidi;
        if (sends) midiDelivery_.begin(midiOutput.destination ? &midiOutput : nullptr, std::uint32_t(data.numSamples));
        if (descriptor.supportsSysEx && midiOutput.destination) {
            sysExOutput_.clear();
            midiOutput.sysEx = &sysExOutput_;
        }
        const cell::AudioBlock<float> audio{output,channels_,std::uint32_t(data.numSamples),0};
        engine_.process(audio,{events.data(),count},transport,{midi_.data(),midiCount},side,sends?&midiDelivery_:nullptr);
#if defined(CELL_VST3_WEB_EDITOR)
        if constexpr (designerHasOutputMeter) editorMeter_.publish(audio);
#endif
        sendGenerated(data.outputParameterChanges);sendAnalysis();data.outputs[0].silenceFlags=0;return kResultOk;
    }
private:
#if defined(CELL_VST3_WEB_EDITOR)
    tresult handleDesignerCommand(IMessage* message) {
        auto* attributes = message->getAttributes();
        int64 session{}, operation{};
        if (!attributes || attributes->getInt("session", session) != kResultOk || session <= 0 ||
            attributes->getInt("operation", operation) != kResultOk || operation < int64(DesignerOperation::poll) ||
            operation > int64(DesignerOperation::close)) return kInvalidArgument;
        if (session < commandSession_ || (session == commandSession_ && commandClosed_)) return kResultOk;
        if (session != commandSession_) {
            designerCommands_.cancel();
            commandSession_ = session;
            commandClosed_ = false;
        }
        bool changed{};
        if (operation == int64(DesignerOperation::apply)) {
            const void* bytes{};
            uint32 size{};
            if (attributes->getBinary("command", bytes, size) != kResultOk || !bytes || !size ||
                size > designerCommandBytes) return kInvalidArgument;
            changed = designerCommands_.apply({static_cast<const char*>(bytes), size});
        } else if (operation != int64(DesignerOperation::poll)) {
            designerCommands_.cancel();
            commandClosed_ = operation == int64(DesignerOperation::close);
        }
        std::vector<DesignerEdit> edits;
        for (cell::ParameterEdit edit; engine_.pollUiEdit(edit);)
            edits.push_back({edit.id, static_cast<uint32>(edit.type), edit.value});

        const auto snapshot = designerCommands_.snapshot();
        auto reply = owned(allocateMessage());
        if (!reply) return kOutOfMemory;
        reply->setMessageID(designerCommandReply);
        auto* data = reply->getAttributes();
        if (!data || data->setInt("session", session) != kResultOk ||
            data->setInt("changed", changed ? 1 : 0) != kResultOk ||
            data->setInt("edits", int64(edits.size())) != kResultOk ||
            data->setBinary("snapshot", snapshot.data(), uint32(snapshot.size())) != kResultOk ||
            (!edits.empty() && data->setBinary("values", edits.data(), uint32(edits.size() * sizeof(DesignerEdit))) != kResultOk))
            return kResultFalse;
        return sendMessage(reply);
    }

    cell::web::OutputMeter editorMeter_;
    int64 meterSession_{};
    bool meterClosed_{};
    int64 commandSession_{};
    bool commandClosed_{};
#endif
    cell::midi::Delivery midiDelivery_;
    cell::midi::SysExStorage sysExOutput_;
    bool loadProductBytes(std::span<const std::byte> bytes){cell::CallbackGate::PauseScope pause(callbacks_,engine_.stateRequiresStoppedProcessing());return pause&&engine_.loadState(bytes);}
    void sendGenerated(IParameterChanges* changes)noexcept{
        if(!changes)return;
        cell::ParameterEdit edit;
        while(hasPendingGenerated_||engine_.pollGenerated(edit)){
            if(hasPendingGenerated_)edit=pendingGenerated_;
            const auto* field=engine_.parameters().find(edit.id);
            if(!field){hasPendingGenerated_=false;continue;}
            int32 queueIndex{},pointIndex{};auto* queue=changes->addParameterData(edit.id,queueIndex);
            if(!queue||queue->addPoint(0,field->toNormalized(edit.value),pointIndex)!=kResultOk){pendingGenerated_=edit;hasPendingGenerated_=true;return;}
            hasPendingGenerated_=false;
        }
    }
    void updateAnalysisState(){
        const int available=exchange_&&queue_!=InvalidDataExchangeQueueID?1:setupValid_&&connected_&&analysis_?2:0;
        if(analysis_)analysis_->enableAnalysis(analysisRequested_&&available);
        if(auto message=owned(allocateMessage())){message->setMessageID(cell::analysisStatusMessage);message->getAttributes()->setInt("available",available);message->getAttributes()->setInt("latency",engine_.processor().latency());sendMessage(message);}
    }
    void openAnalysisQueue(){
        if(active_||!setupValid_||!connected_||queue_!=InvalidDataExchangeQueueID)return;
        FUnknownPtr<IDataExchangeHandler> handler(getHostContext());
        if(handler&&handler->openQueue(this,sizeof(cell::AnalysisFrame),3,32,cell::analysisContextId,&queue_)==kResultOk)exchange_=handler;
        else queue_=InvalidDataExchangeQueueID;
        updateAnalysisState();
    }
    void closeAnalysisQueue(){
        if(analysis_)analysis_->enableAnalysis(false);
        if(exchange_&&queue_!=InvalidDataExchangeQueueID)exchange_->closeQueue(queue_);
        queue_=InvalidDataExchangeQueueID;exchange_=nullptr;
        if(analysis_){cell::AnalysisFrame discarded;while(analysis_->pullAnalysis(discarded)){} }
    }
    void sendAnalysis()noexcept{
        if(!exchange_||queue_==InvalidDataExchangeQueueID)return;
        cell::AnalysisFrame latest;bool received{};if(analysis_)for(unsigned n=0;n<3;++n){cell::AnalysisFrame next;if(!analysis_->pullAnalysis(next))break;latest=next;received=true;}
        const auto currentLatency=engine_.processor().latency();if(!received&&currentLatency==reportedLatency_)return;
        if(!received){latest.flags=1;latest.sampleRate=sampleRate_;}latest.latency=currentLatency;
        DataExchangeBlock block{};if(exchange_->lockBlock(queue_,&block)!=kResultOk)return;
        if(block.data&&block.size>=sizeof(latest)){std::memcpy(block.data,&latest,sizeof(latest));exchange_->freeBlock(queue_,block.blockID,true);reportedLatency_=currentLatency;}
        else exchange_->freeBlock(queue_,block.blockID,false);
    }
    cell::Engine engine_{cell::plugin::descriptor(),cell::plugin::createProcessor()};
#if defined(CELL_VST3_WEB_EDITOR)
    cell::web::ParameterBridge designerCommands_{engine_, {}, true
#if defined(CELL_DESIGNER_HAS_ACTIONS)
        , cell::designer::actions
#endif
    };
#endif
    std::array<cell::ParameterEvent,4096> events_{},eventScratch_{};std::array<cell::MidiEvent,2048> midi_{},midiScratch_{};
    InputNotes inputNotes_;
    std::uint32_t channels_=cell::plugin::descriptor().channels;
    std::uint32_t inputChannels_=cell::plugin::descriptor().channels;
    cell::CallbackGate callbacks_;
    cell::ParameterEdit pendingGenerated_{};bool hasPendingGenerated_{};
    cell::AnalysisSource* analysis_{};
    IPtr<IDataExchangeHandler> exchange_;
    DataExchangeQueueID queue_=InvalidDataExchangeQueueID;
    bool connected_{},setupValid_{},activationPrepared_{},active_{},analysisRequested_{};std::uint32_t reportedLatency_{},maximumFrames_{};double sampleRate_=48000;
};

class CellParameter final : public Steinberg::Vst::Parameter {
public:
    explicit CellParameter(const cell::Parameter& parameter,cell::ParameterPresentationSource* source=nullptr):parameter_(parameter),display_(parameter),source_(source){info.id=parameter.id;refresh();setNormalized(info.defaultNormalizedValue);}
    void refresh(){display_=parameter_;bool hidden=false;if(source_)if(auto p=source_->parameterPresentation(parameter_.id)){display_=std::move(p->field);hidden=p->hidden;}
        UString(info.title,128).fromAscii(display_.name.c_str());UString(info.shortTitle,128).fromAscii(display_.name.c_str());UString(info.units,128).fromAscii(display_.unit.c_str());info.defaultNormalizedValue=display_.toNormalized(display_.defaultValue);info.stepCount=display_.step>=1?int32((display_.maximum-display_.minimum)/display_.step):0;info.flags=(display_.automatable?ParameterInfo::kCanAutomate:0)|(parameter_.bypass?ParameterInfo::kIsBypass:0)|(hidden?ParameterInfo::kIsHidden:0);info.unitId=kRootUnitId;
    }
    ParamValue toPlain(ParamValue value)const override{return display_.fromNormalized(value);}
    ParamValue toNormalized(ParamValue value)const override{return display_.toNormalized(value);}
    void toString(ParamValue value,String128 text)const override{if(!display_.choices.empty()){const auto valueText=display_.text(toPlain(value));UString(text,128).fromAscii(valueText.c_str());}else UString(text,128).printFloat(toPlain(value),display_.step>=1?0:2);}
    bool fromString(const TChar* text,ParamValue& value)const override{char ascii[256]{};UString(const_cast<TChar*>(text),128).toAscii(ascii,256);if(!display_.choices.empty())for(unsigned n=0;n<display_.choices.size();++n)if(display_.choices[n]==ascii){value=toNormalized(display_.minimum+n);return true;}double plain{};if(!UString(const_cast<TChar*>(text),128).scanFloat(plain))return false;value=toNormalized(plain);return true;}
private:const cell::Parameter& parameter_;cell::Parameter display_;cell::ParameterPresentationSource* source_{};
};

class Controller final : public EditController,public IDataExchangeReceiver,public IMidiMapping,public INoteExpressionController {
public:
    Controller(){if(auto* source=analysis())source->setAnalysisConnection(cell::AnalysisConnection::waiting);}
    static FUnknown* create(void*){
        return cell::plugin::descriptor().channels ? static_cast<IEditController*>(new Controller) : nullptr;
    }
    tresult PLUGIN_API initialize(FUnknown* context)override {
        const auto result=EditController::initialize(context);if(result!=kResultOk)return result;
        if(auto* extension=dynamic_cast<cell::NativeHostContext*>(&engine.processor()))extension->nativeHostContext("VST3",context,false);
        if(auto* extension=dynamic_cast<cell::HostProjectContext*>(&engine.processor()))if(!extension->hostProjectContext(cell::vst3::projectIdentity(context)))return kResultFalse;
        for(const auto& p:cell::plugin::descriptor().parameters){if(cell::plugin::descriptor().acceptsMidi&&(p.id&0xfffff000u)==0x7f700000u)return kResultFalse;parameters.addParameter(new CellParameter(p,dynamic_cast<cell::ParameterPresentationSource*>(&engine.processor())));}
        if(cell::plugin::descriptor().acceptsMidi)for(unsigned channel=0;channel<16;++channel)for(unsigned cc=0;cc<130;++cc){String128 title;const auto text="MIDI "+std::to_string(channel+1)+" / "+(cc<128?"CC "+std::to_string(cc):cc==128?"AFTERTOUCH":"PITCH BEND");UString(title,128).fromAscii(text.c_str());ParameterInfo info{};info.id=0x7f700000u|(channel<<8)|cc;UString(info.title,128).fromAscii(text.c_str());info.stepCount=cc==129?16383:127;info.defaultNormalizedValue=cc==129?.5:0;info.flags=ParameterInfo::kIsHidden;parameters.addParameter(info);}
        return kResultOk;
    }
    tresult PLUGIN_API terminate()override{if(auto* extension=dynamic_cast<cell::NativeHostContext*>(&engine.processor()))extension->nativeHostContext("VST3",nullptr,false);return EditController::terminate();}
    tresult PLUGIN_API setComponentState(IBStream* state)override {
        try{if(!readState(state,engine))return kResultFalse;productQueries_.clear();refreshParameterPresentation();
            for(const auto& p:cell::plugin::descriptor().parameters)EditController::setParamNormalized(p.id,p.toNormalized(engine.parameters().get(p.id)));
            return kResultOk;}catch(...){return kResultFalse;}
    }
    tresult PLUGIN_API setParamNormalized(ParamID id,ParamValue value)override {
        if(cell::plugin::descriptor().acceptsMidi&&(id&0xfffff000u)==0x7f700000u&&(id&255)<130)return EditController::setParamNormalized(id,value);
        const auto* p=engine.parameters().find(id);if(!p)return kInvalidArgument;
        engine.parameters().set(id,p->fromNormalized(value));return EditController::setParamNormalized(id,value);
    }
    void refreshParameterPresentation(){auto* source=dynamic_cast<cell::ParameterPresentationSource*>(&engine.processor());if(!source)return;const auto revision=source->parameterPresentationRevision();if(revision==presentationRevision_)return;for(const auto& field:cell::plugin::descriptor().parameters)if(auto* parameter=dynamic_cast<CellParameter*>(parameters.getParameter(field.id)))parameter->refresh();presentationRevision_=revision;if(componentHandler)componentHandler->restartComponent(kParamTitlesChanged|kParamValuesChanged);}
    bool hasParameterMenu(cell::ParamId id){const auto* field=engine.parameters().find(id);FUnknownPtr<IComponentHandler3> handler(componentHandler.get());return field&&field->automatable&&handler;}
    bool parameterMenu(IPlugView* view,cell::ParamId id,double x,double y){
        if(!view||!hasParameterMenu(id)||!std::isfinite(x)||!std::isfinite(y)||x<0||y<0||x>INT32_MAX||y>INT32_MAX)return false;
        FUnknownPtr<IComponentHandler3> handler(componentHandler.get());auto menu=owned(handler->createContextMenu(view,&id));
        return menu&&menu->popup(UCoord(x),UCoord(y))==kResultOk;
    }
    void reportEdit(cell::ParameterEdit edit){const auto* p=engine.parameters().find(edit.id);if(!p)return;
            if(edit.type==cell::EditType::begin)beginEdit(edit.id);
            else if(edit.type==cell::EditType::end)endEdit(edit.id);
            else{const auto value=p->toNormalized(edit.value);setParamNormalized(edit.id,value);performEdit(edit.id,value);}
    }
    void flushEdits(){cell::ParameterEdit edit;
        while(engine.pollEdit(edit))reportEdit(edit);
    }
#if defined(CELL_VST3_WEB_EDITOR)
    int64 beginDesignerCommands() {
        const std::lock_guard lock(designerReplyMutex_);
        designerReplies_.clear();
        designerProgramState_ = {};
        nextDesignerPoll_ = {};
        return ++designerSession_;
    }

    bool sendDesignerCommand(int64 session, DesignerOperation operation, std::string_view command = {}) {
        if (session != designerSession_ || command.size() > designerCommandBytes) return false;
        auto request = owned(allocateMessage());
        if (!request) return false;
        request->setMessageID(designerCommandRequest);
        auto* data = request->getAttributes();
        return data && data->setInt("session", session) == kResultOk &&
            data->setInt("operation", int64(operation)) == kResultOk &&
            (command.empty() || data->setBinary("command", command.data(), uint32(command.size())) == kResultOk) &&
            sendMessage(request) == kResultOk;
    }

    void flushDesignerCommands(int64 session) {
        std::vector<DesignerReply> pending;
        {
            const std::lock_guard lock(designerReplyMutex_);
            if (session != designerSession_) return;
            pending.swap(designerReplies_);
        }
        for (const auto& reply : pending) {
            const auto snapshot = cell::json::read(reply.snapshot, {designerSnapshotBytes, 4096, designerSnapshotBytes, 16});
            if (snapshot["type"].text() != "parameters") continue;
            designerProgramState_ = snapshot;
            for (const auto& edit : reply.edits) {
                const auto* parameter = engine.parameters().find(edit.id);
                if (!parameter || edit.type > static_cast<uint32>(cell::EditType::end) || !std::isfinite(edit.value))
                    continue;
                reportEdit({static_cast<cell::EditType>(edit.type), edit.id, edit.value});
            }
            if (reply.changed)
                if (FUnknownPtr<IComponentHandler2> handler{componentHandler.get()}) handler->setDirty(true);
        }
    }

    void refreshDesignerCommands(int64 session) {
        if (session != designerSession_) return;
        const auto now = std::chrono::steady_clock::now();
        if (now >= nextDesignerPoll_) {
            nextDesignerPoll_ = now + std::chrono::milliseconds(100);
            sendDesignerCommand(session, DesignerOperation::poll);
        }
        flushDesignerCommands(session);
    }

    std::string designerSnapshot(int64 session, std::string parameters) {
        if (session != designerSession_) return parameters;
        if (designerProgramState_["programs"].isNull() && designerProgramState_["actions"].isNull()) return parameters;
        auto local = cell::json::read(parameters, {designerSnapshotBytes, 4096, designerSnapshotBytes, 16});
        auto fields = *local.object();
        for (const auto* name : {"programs", "actions"})
            if (!designerProgramState_[name].isNull()) fields.emplace(name, designerProgramState_[name]);
        return cell::json::write(cell::json::Value(std::move(fields)), {designerSnapshotBytes, 8192, designerSnapshotBytes, 16});
    }

    void designerMeterVisible(bool visible) {
        if (visible) {
            if (++meterViews_ != 1) return;
            meterSession_ = meterSession_ == INT64_MAX ? 1 : meterSession_ + 1;
            meterLevels_ = {};
            nextMeterPoll_ = {};
        } else if (meterViews_ && --meterViews_ == 0) {
            requestDesignerMeter(false);
        }
    }

    std::string designerMeterSnapshot() {
        const auto now = std::chrono::steady_clock::now();
        if (meterViews_ && now >= nextMeterPoll_) {
            nextMeterPoll_ = now + std::chrono::milliseconds(33);
            requestDesignerMeter(true);
        }
        return cell::web::OutputMeter::snapshot(meterLevels_);
    }
#endif
    bool command(std::span<const std::byte> bytes){if(bytes.empty()||bytes.size()>16384)return false;auto message=owned(allocateMessage());if(!message)return false;message->setMessageID(cell::productCommandMessage);message->getAttributes()->setBinary("command",bytes.data(),uint32(bytes.size()));if(sendMessage(message)!=kResultOk)return false;if(FUnknownPtr<IComponentHandler2> handler{componentHandler.get()})handler->setDirty(true);return true;}
    bool input(std::span<const std::byte> bytes){if(bytes.empty()||bytes.size()>cell::maxProductQueryBytes)return false;auto message=owned(allocateMessage());if(!message)return false;message->setMessageID(cell::productInputMessage);message->getAttributes()->setBinary("command",bytes.data(),uint32(bytes.size()));return sendMessage(message)==kResultOk;}
    std::vector<std::byte> query(std::span<const std::byte> bytes,bool fresh){
        if(fresh)productQueries_.invalidate(bytes);
        auto previous=productQueries_.take(bytes);if(!previous.empty())return previous;auto request=productQueries_.begin(bytes);if(!request)return previous;
        auto message=owned(allocateMessage());if(message){message->setMessageID(cell::productQueryMessage);message->getAttributes()->setInt("token",int64(request->token));message->getAttributes()->setBinary("query",request->bytes.data(),uint32(request->bytes.size()));}
        if(!message||sendMessage(message)!=kResultOk)productQueries_.complete(request->token,{});
        auto immediate=productQueries_.take(bytes);return immediate.empty()?std::move(previous):std::move(immediate);
    }
    std::uint64_t requestRestore(std::span<const std::byte> bytes){
        if(bytes.empty()||bytes.size()>128u*1024*1024)return 0;
        auto message=owned(allocateMessage());if(!message)return 0;
        auto request=restoreAcks_.begin(bytes,128u*1024*1024);if(!request.token)return 0;
        message->setMessageID(cell::productRestoreMessage);message->getAttributes()->setInt("token",int64(request.token));message->getAttributes()->setBinary("state",request.bytes->data(),uint32(request.bytes->size()));
        if(sendMessage(message)!=kResultOk)restoreAcks_.acknowledge(request.token,false);
        return request.token;
    }
    cell::RestoreStatus restoreStatus(std::uint64_t token){
        if(auto receipt=restoreAcks_.consume(token)){
            bool applied=false;try{applied=receipt->accepted&&engine.loadState(*receipt->request.bytes);}catch(...){applied=false;}
            restoreAcks_.finish(token,applied);
            if(applied){productQueries_.clear();refreshParameterPresentation();for(const auto& p:cell::plugin::descriptor().parameters)EditController::setParamNormalized(p.id,p.toNormalized(engine.parameters().get(p.id)));if(componentHandler)componentHandler->restartComponent(kParamValuesChanged);if(FUnknownPtr<IComponentHandler2> handler{componentHandler.get()})handler->setDirty(true);}
        }
        return restoreAcks_.status(token);
    }
    std::uint64_t requestCommand(std::span<const std::byte> bytes){
        auto message=owned(allocateMessage());if(!message)return 0;auto request=commandAcks_.begin(bytes,cell::maxProductQueryBytes);if(!request.token)return 0;
        message->setMessageID(cell::productAcknowledgedCommandMessage);message->getAttributes()->setInt("token",int64(request.token));message->getAttributes()->setBinary("command",request.bytes->data(),uint32(request.bytes->size()));
        if(sendMessage(message)!=kResultOk)commandAcks_.acknowledge(request.token,false);return request.token;
    }
    cell::RestoreStatus commandStatus(std::uint64_t token){if(auto receipt=commandAcks_.consume(token)){commandAcks_.finish(token,receipt->accepted);if(receipt->accepted)if(FUnknownPtr<IComponentHandler2> handler{componentHandler.get()})handler->setDirty(true);}return commandAcks_.status(token);}
    bool restoreProduct(std::span<const std::byte> bytes){
        if(bytes.size()>128*1024*1024)return false;auto previous=engine.saveState();if(!engine.loadState(bytes))return false;
        auto message=owned(allocateMessage());if(!message){engine.loadState(previous);return false;}message->setMessageID("Cell.State.Restore.v1");message->getAttributes()->setBinary("state",bytes.data(),uint32(bytes.size()));
        if(sendMessage(message)!=kResultOk){engine.loadState(previous);return false;}productQueries_.clear();
        refreshParameterPresentation();for(const auto& p:cell::plugin::descriptor().parameters)EditController::setParamNormalized(p.id,p.toNormalized(engine.parameters().get(p.id)));
        if(componentHandler)componentHandler->restartComponent(kParamValuesChanged);
        if(FUnknownPtr<IComponentHandler2> handler{componentHandler.get()})handler->setDirty(true);return true;
    }
    cell::AnalysisSource* analysis(){return dynamic_cast<cell::AnalysisSource*>(&engine.processor());}
    void analysisUi(bool enabled){
        if(!analysis())return;analysisVisible_=enabled;
        if(auto message=owned(allocateMessage())){message->setMessageID(cell::analysisEnableMessage);message->getAttributes()->setInt("enabled",enabled?1:0);sendMessage(message);}
    }
    void pollAnalysis(){
        if(!analysisVisible_||!pollingAnalysis_)return;const auto now=std::chrono::steady_clock::now();if(now-lastPoll_<std::chrono::milliseconds(25))return;lastPoll_=now;
        if(auto message=owned(allocateMessage())){message->setMessageID("Cell.Analysis.Poll.v4");sendMessage(message);}
    }
    void receiveFrame(const cell::AnalysisFrame& frame){
        if(frame.version!=cell::analysisVersion||frame.count!=cell::analysisSize||frame.flags>1||!std::isfinite(frame.sampleRate)||frame.sampleRate<1000||frame.sampleRate>768000)return;
        if(frame.latency!=knownLatency_){knownLatency_=frame.latency;if(componentHandler)componentHandler->restartComponent(kLatencyChanged);}
        if(!frame.flags)if(auto* source=analysis())source->receiveAnalysis(frame);
    }
    tresult PLUGIN_API notify(IMessage* message)override{
#if defined(CELL_VST3_WEB_EDITOR)
        if (message && message->getMessageID() && !std::strcmp(message->getMessageID(), designerCommandReply)) {
            try { return receiveDesignerReply(message); }
            catch (...) { return kResultFalse; }
        }
        if (message && message->getMessageID() && !std::strcmp(message->getMessageID(), designerMeterReadings)) {
            auto* attributes = message->getAttributes();
            int64 session{};
            double peak{}, rms{};
            if (!attributes || attributes->getInt("session", session) != kResultOk ||
                attributes->getFloat("peak", peak) != kResultOk || attributes->getFloat("rms", rms) != kResultOk ||
                !std::isfinite(peak) || !std::isfinite(rms) || peak < 0 || rms < 0) return kInvalidArgument;
            if (meterViews_ && session == meterSession_)
                meterLevels_ = {static_cast<float>(std::min(peak, 1.0)), static_cast<float>(std::min(rms, 1.0))};
            return kResultOk;
        }
#endif
        if(message&&message->getMessageID()&&(!std::strcmp(message->getMessageID(),cell::productRestoreReplyMessage)||!std::strcmp(message->getMessageID(),cell::productCommandReplyMessage))){
            const bool restore=!std::strcmp(message->getMessageID(),cell::productRestoreReplyMessage);int64 token{},accepted{};auto* attributes=message->getAttributes();
            if(!attributes||attributes->getInt("token",token)!=kResultOk||token<=0||attributes->getInt(restore?"restored":"accepted",accepted)!=kResultOk||(accepted!=0&&accepted!=1))return kInvalidArgument;
            return (restore?restoreAcks_:commandAcks_).acknowledge(std::uint64_t(token),accepted!=0)?kResultOk:kResultFalse;
        }
        if(message&&message->getMessageID()&&!std::strcmp(message->getMessageID(),cell::productQueryReplyMessage)){
            auto* attributes=message->getAttributes();int64 token{},declared{};const void* data{};uint32 size{};
            if(!attributes||attributes->getInt("token",token)!=kResultOk||token<=0||attributes->getInt("size",declared)!=kResultOk||declared<0||declared>int64(cell::maxProductReplyBytes))return kInvalidArgument;
            if(declared&&(attributes->getBinary("data",data,size)!=kResultOk||!data||size!=uint32(declared)))return kInvalidArgument;
            return productQueries_.complete(std::uint64_t(token),{static_cast<const std::byte*>(data),size})?kResultOk:kResultFalse;
        }
        if(message&&message->getMessageID()&&!std::strcmp(message->getMessageID(),"Cell.Analysis.Frame.v4")){
            const void* bytes{};uint32 size{};auto* attributes=message->getAttributes();if(!attributes||attributes->getBinary("frame",bytes,size)!=kResultOk||!bytes||size!=sizeof(cell::AnalysisFrame))return kInvalidArgument;
            cell::AnalysisFrame frame;std::memcpy(&frame,bytes,sizeof frame);receiveFrame(frame);return kResultOk;
        }
        if(message&&message->getMessageID()&&!std::strcmp(message->getMessageID(),cell::analysisStatusMessage)){
            int64 available{};auto* attributes=message->getAttributes();
            if(!attributes||attributes->getInt("available",available)!=kResultOk)return kInvalidArgument;
            int64 latency{};if(attributes->getInt("latency",latency)==kResultOk&&latency>=0&&latency<=UINT32_MAX)knownLatency_=std::uint32_t(latency);
            pollingAnalysis_=available==2;
            if(auto* source=analysis())source->setAnalysisConnection(available?cell::AnalysisConnection::host:cell::AnalysisConnection::unavailable);
            return kResultOk;
        }
        return EditController::notify(message);
    }
    void PLUGIN_API queueOpened(DataExchangeUserContextID context,uint32 size,TBool& background)override{
        background=false;
        if(auto* source=analysis();source&&context==cell::analysisContextId&&size==sizeof(cell::AnalysisFrame))source->setAnalysisConnection(cell::AnalysisConnection::host);
    }
    void PLUGIN_API queueClosed(DataExchangeUserContextID context)override{
        if(auto* source=analysis();source&&context==cell::analysisContextId)source->setAnalysisConnection(cell::AnalysisConnection::waiting);
    }
    void PLUGIN_API onDataExchangeBlocksReceived(DataExchangeUserContextID context,uint32 count,DataExchangeBlock* blocks,TBool background)override{
        if(background||context!=cell::analysisContextId||!blocks)return;
        for(uint32 n=0;n<count;++n)if(blocks[n].data&&blocks[n].size==sizeof(cell::AnalysisFrame)){
            cell::AnalysisFrame frame;std::memcpy(&frame,blocks[n].data,sizeof(frame));
            receiveFrame(frame);
        }
    }
    tresult PLUGIN_API getMidiControllerAssignment(int32 bus,int16 channel,CtrlNumber cc,ParamID& id)override{if(!cell::plugin::descriptor().acceptsMidi||bus!=0||channel<0||channel>=16||cc<0||cc>=130)return kResultFalse;id=0x7f700000u|(unsigned(channel)<<8)|unsigned(cc);return kResultOk;}
    int32 PLUGIN_API getNoteExpressionCount(int32 bus, int16 channel) override {
        const auto& descriptor = cell::plugin::descriptor();
        return descriptor.acceptsMidi && descriptor.supportsUmp && bus == 0 && channel >= 0 && channel < 16
            ? 1 + controllersPerKind * 2 : 0;
    }
    tresult PLUGIN_API getNoteExpressionInfo(int32 bus, int16 channel, int32 index,
                                           NoteExpressionTypeInfo& info) override {
        if (index < 0 || index >= getNoteExpressionCount(bus, channel)) return kResultFalse;
        info = {};
        info.unitId = kRootUnitId;
        if (index == 0) {
            info.typeId = kTuningTypeID;
            UString(info.title, 128).fromAscii("Tuning");
            UString(info.shortTitle, 128).fromAscii("Tuning");
            UString(info.units, 128).fromAscii("semitones");
            info.valueDesc = {.5, 0, 1, 0};
            info.flags = NoteExpressionTypeInfo::kIsBipolar;
        } else {
            const bool registered = index <= controllersPerKind;
            const auto number = (index - 1) % controllersPerKind;
            info.typeId = (registered ? registeredStart : assignableStart) + number;
            const auto title = std::string(registered ? "Registered " : "Assignable ") + std::to_string(number);
            UString(info.title, 128).fromAscii(title.c_str());
            UString(info.shortTitle, 128).fromAscii(title.c_str());
            info.valueDesc = {0, 0, 1, 0};
        }
        return kResultOk;
    }
    tresult PLUGIN_API getNoteExpressionStringByValue(int32 bus, int16 channel, NoteExpressionTypeID id,
                                                    NoteExpressionValue value, String128 text) override {
        if (!hasNoteExpression(bus, channel, id) || !std::isfinite(value) || value < 0 || value > 1)
            return kResultFalse;
        UString(text, 128).printFloat(id == kTuningTypeID ? 240 * (value - .5) : value, 4);
        return kResultOk;
    }
    tresult PLUGIN_API getNoteExpressionValueByString(int32 bus, int16 channel, NoteExpressionTypeID id,
                                                    const TChar* text, NoteExpressionValue& value) override {
        double plain{};
        if (!hasNoteExpression(bus, channel, id) || !text ||
            !UString(const_cast<TChar*>(text), 128).scanFloat(plain)) return kResultFalse;
        const auto normalized = id == kTuningTypeID ? plain / 240 + .5 : plain;
        if (!std::isfinite(normalized) || normalized < 0 || normalized > 1) return kResultFalse;
        value = normalized;
        return kResultOk;
    }
    bool hasNoteExpression(int32 bus, int16 channel, NoteExpressionTypeID id) {
        return getNoteExpressionCount(bus, channel) && (id == kTuningTypeID ||
            (id >= registeredStart && id < registeredStart + controllersPerKind) ||
            (id >= assignableStart && id < assignableStart + controllersPerKind));
    }
    OBJ_METHODS(Controller,EditController)
    DEFINE_INTERFACES
        DEF_INTERFACE(IDataExchangeReceiver)
        DEF_INTERFACE(IMidiMapping)
        DEF_INTERFACE(INoteExpressionController)
    END_DEFINE_INTERFACES(EditController)
    REFCOUNT_METHODS(EditController)
    IPlugView* PLUGIN_API createView(FIDString type)override;
    cell::Engine engine{cell::plugin::descriptor(),cell::plugin::createProcessor()};
    std::uint64_t presentationRevision_{};
    cell::OperationAck restoreAcks_,commandAcks_;
    cell::ProductQueries productQueries_;unsigned views{};std::uint32_t knownLatency_{};bool analysisVisible_{},pollingAnalysis_{};std::chrono::steady_clock::time_point lastPoll_{};
#if defined(CELL_VST3_WEB_EDITOR)
private:
    tresult receiveDesignerReply(IMessage* message) {
        auto* data = message->getAttributes();
        int64 session{}, changed{}, edits{};
        const void* snapshot{}, *values{};
        uint32 length{}, valuesLength{};
        if (!data || data->getInt("session", session) != kResultOk || session <= 0 ||
            data->getInt("changed", changed) != kResultOk || (changed != 0 && changed != 1) ||
            data->getBinary("snapshot", snapshot, length) != kResultOk || !snapshot ||
            !length || length > designerSnapshotBytes ||
            data->getInt("edits", edits) != kResultOk || edits < 0 || edits > int64(designerMaxEdits) ||
            (edits && (data->getBinary("values", values, valuesLength) != kResultOk || !values ||
                       valuesLength != std::size_t(edits) * sizeof(DesignerEdit))))
            return kInvalidArgument;
        const std::lock_guard lock(designerReplyMutex_);
        if (session != designerSession_) return kResultOk;
        if (designerReplies_.size() == 64) return kResultFalse;
        DesignerReply reply{std::string(static_cast<const char*>(snapshot), length),
                            std::vector<DesignerEdit>(static_cast<std::size_t>(edits)), changed != 0};
        if (edits) std::memcpy(reply.edits.data(), values, valuesLength);
        designerReplies_.push_back(std::move(reply));
        return kResultOk;
    }

    std::mutex designerReplyMutex_;
    int64 designerSession_{};
    std::vector<DesignerReply> designerReplies_;
    cell::json::Value designerProgramState_;
    std::chrono::steady_clock::time_point nextDesignerPoll_{};

    void requestDesignerMeter(bool visible) {
        auto request = owned(allocateMessage());
        if (!request) return;
        request->setMessageID(designerMeterRequest);
        auto* attributes = request->getAttributes();
        if (attributes && attributes->setInt("session", meterSession_) == kResultOk &&
            attributes->setInt("visible", visible ? 1 : 0) == kResultOk) sendMessage(request);
    }

    unsigned meterViews_{};
    int64 meterSession_{};
    cell::web::OutputMeter::Levels meterLevels_{};
    std::chrono::steady_clock::time_point nextMeterPoll_{};
#endif
};

#if CELL_HAS_UI
class View final : public CPluginView,public IPlugViewContentScaleSupport
#if SMTG_OS_LINUX
    ,public Linux::ITimerHandler
#endif
{
public:
    explicit View(Controller& controller):controller_(&controller),editor_(cell::plugin::createEditor(controller.engine)),window_(*editor_),geometry_(editor_->preferredSize()) {
        editor_->remoteProductState=true;
        const auto preferred=editor_->preferredSize();rect={0,0,int32(preferred.x),int32(preferred.y)};editor_->editsAvailable=[this]{controller_->flushEdits();};
        editor_->synchronize=[this]{controller_->pollAnalysis();};
        editor_->visibilityChanged=[this](bool visible){controller_->analysisUi(visible);};++controller_->views;
        editor_->requestSize=[this](int w,int h){
            if(!plugFrame)return false;auto candidate=geometry_;if(!candidate.setLogicalSize({double(w),double(h)}))return false;
            ViewRect next{};if(!scaledRect(candidate.nativeSize(),rect,next))return false;
            const auto previous=geometry_;geometry_=candidate;
            if(plugFrame->resizeView(this,&next)==kResultOk)return true;
            geometry_=previous;return false;
        };
        editor_->productCommand=[this](std::span<const std::byte> bytes){return controller_->command(bytes);};
        editor_->transientInput=[this](std::span<const std::byte> bytes){return controller_->input(bytes);};
        editor_->productQuery=[this](std::span<const std::byte> bytes,bool fresh){return controller_->query(bytes,fresh);};
        editor_->restoreProductState=[this](std::span<const std::byte> bytes){return controller_->restoreProduct(bytes);};
        editor_->requestProductRestore=[this](std::span<const std::byte> bytes){return controller_->requestRestore(bytes);};
        editor_->productRestoreStatus=[this](std::uint64_t token){return controller_->restoreStatus(token);};
        editor_->requestProductCommand=[this](std::span<const std::byte> bytes){return controller_->requestCommand(bytes);};
        editor_->productCommandStatus=[this](std::uint64_t token){return controller_->commandStatus(token);};
        editor_->hostParameterMenuAvailable=[this](cell::ParamId id){return window_.isOpen()&&controller_->hasParameterMenu(id);};
        editor_->showHostParameterMenu=[this](cell::ParamId id,cell::ui::Point at){const auto point=window_.toNativePoint(at);return window_.isOpen()&&controller_->parameterMenu(this,id,point.x,point.y);};
    }
    ~View()override{detach();editor_->visibilityChanged={};editor_->synchronize={};--controller_->views;}
    tresult PLUGIN_API isPlatformTypeSupported(FIDString type)override {
#if SMTG_OS_WINDOWS
        return type&&!std::strcmp(type,kPlatformTypeHWND)?kResultTrue:kResultFalse;
#elif SMTG_OS_MACOS
        return type&&!std::strcmp(type,kPlatformTypeNSView)?kResultTrue:kResultFalse;
#else
        return type&&!std::strcmp(type,kPlatformTypeX11EmbedWindowID)?kResultTrue:kResultFalse;
#endif
    }
    tresult PLUGIN_API attached(void* parent,FIDString type)override {
        if(!parent||isPlatformTypeSupported(type)!=kResultTrue)return kResultFalse;
        try{
#if SMTG_OS_LINUX
            if(!plugFrame||plugFrame->queryInterface(Linux::IRunLoop::iid,reinterpret_cast<void**>(&runLoop_))!=kResultOk)return kResultFalse;
            if(runLoop_->registerTimer(this,33)!=kResultOk){runLoop_->release();runLoop_=nullptr;return kResultFalse;}
#endif
            if(!window_.open(reinterpret_cast<std::uintptr_t>(parent),rect.getWidth(),rect.getHeight())){detach();return kResultFalse;}
            window_.show(true);window_.poll();return CPluginView::attached(parent,type);
        }catch(...){detach();return kResultFalse;}
    }
    tresult PLUGIN_API removed()override{detach();return CPluginView::removed();}
    tresult PLUGIN_API onKeyDown(char16 key,int16 code,int16 modifiers)override{return editor_->keyboard(keyEvent(key,code,modifiers))?kResultTrue:kResultFalse;}
    tresult PLUGIN_API onKeyUp(char16 key,int16 code,int16 modifiers)override{return editor_->keyReleased(keyEvent(key,code,modifiers))?kResultTrue:kResultFalse;}
    tresult PLUGIN_API onFocus(TBool focused)override{if(!focused)editor_->cancelInput();return kResultTrue;}
    tresult PLUGIN_API onSize(ViewRect* size)override {
        if(!size)return kInvalidArgument;const cell::ui::Point native{double(int64(size->right)-size->left),double(int64(size->bottom)-size->top)};
        auto candidate=geometry_;if(!candidate.resizeNative(native))return kResultFalse;
        if(window_.isOpen()&&!window_.resize(int(native.x),int(native.y)))return kResultFalse;
        geometry_=candidate;
        return CPluginView::onSize(size);
    }
    tresult PLUGIN_API canResize()override{return kResultTrue;}
    tresult PLUGIN_API checkSizeConstraint(ViewRect* size)override {
        if(!size)return kInvalidArgument;
        const cell::ui::Point native{std::clamp(double(int64(size->right)-size->left),1.,cell::ui::maximumNativeExtent),std::clamp(double(int64(size->bottom)-size->top),1.,cell::ui::maximumNativeExtent)};
        const auto next=editor_->constrainSize(cell::ui::logicalPoint(native,geometry_.scale()));ViewRect constrained{};
        if(!scaledRect(cell::ui::ViewGeometry::nativeSize(next,geometry_.scale()),*size,constrained))return kResultFalse;
        *size=constrained;return kResultTrue;
    }
    tresult PLUGIN_API setContentScaleFactor(ScaleFactor factor)override {
        auto candidate=geometry_;if(!candidate.setScale(factor))return kInvalidArgument;
        if(candidate.scale()==geometry_.scale())return kResultTrue;
        ViewRect next{};if(!scaledRect(candidate.nativeSize(),rect,next))return kResultFalse;
        const auto previous=geometry_;const auto oldRect=rect;geometry_=candidate;rect=next;
        if(!window_.setContentScaleFactor(candidate.scale())){geometry_=previous;rect=oldRect;return kResultFalse;}
        if(plugFrame&&plugFrame->resizeView(this,&next)!=kResultOk){geometry_=previous;rect=oldRect;window_.setContentScaleFactor(previous.scale());return kResultFalse;}
        return kResultTrue;
    }
#if SMTG_OS_LINUX
    void PLUGIN_API onTimer()override{window_.poll();}
#endif
    OBJ_METHODS(View,CPluginView)
    DEFINE_INTERFACES
        DEF_INTERFACE(IPlugViewContentScaleSupport)
#if SMTG_OS_LINUX
        DEF_INTERFACE(Linux::ITimerHandler)
#endif
    END_DEFINE_INTERFACES(CPluginView)
    REFCOUNT_METHODS(CPluginView)
private:
    static cell::ui::KeyEvent keyEvent(char16 key,int16 code,int16 modifiers)noexcept{
        int value=int(key);if(!value)switch(code){
            case KEY_BACK:value=8;break;case KEY_TAB:value=9;break;case KEY_RETURN:case KEY_ENTER:value=13;break;case KEY_ESCAPE:value=27;break;case KEY_SPACE:value=32;break;
            case KEY_LEFT:value=cell::ui::keys::left;break;case KEY_RIGHT:value=cell::ui::keys::right;break;case KEY_UP:value=cell::ui::keys::up;break;case KEY_DOWN:value=cell::ui::keys::down;break;case KEY_HOME:value=cell::ui::keys::home;break;case KEY_END:value=cell::ui::keys::end;break;default:break;}
#if SMTG_OS_MACOS
        const bool control=modifiers&kControlKey,command=modifiers&kCommandKey;
#else
        const bool control=modifiers&(kCommandKey|kControlKey),command=false;
#endif
        return {value,bool(modifiers&kShiftKey),control,bool(modifiers&kAlternateKey),command,false};
    }
    static bool scaledRect(cell::ui::Point native,const ViewRect& origin,ViewRect& result)noexcept{
        if(!cell::ui::validNativeSize(native))return false;
        const auto right=int64(origin.left)+int64(native.x),bottom=int64(origin.top)+int64(native.y);
        if(right>INT32_MAX||bottom>INT32_MAX)return false;
        result={origin.left,origin.top,int32(right),int32(bottom)};return true;
    }
    void detach(){
#if SMTG_OS_LINUX
        if(runLoop_){runLoop_->unregisterTimer(this);runLoop_->release();runLoop_=nullptr;}
#endif
        window_.close();
    }
    IPtr<Controller> controller_;std::unique_ptr<cell::ui::Editor> editor_;cell::ui::Window window_;
    cell::ui::ViewGeometry geometry_;
#if SMTG_OS_LINUX
    Linux::IRunLoop* runLoop_{};
#endif
};
#endif

#if defined(CELL_VST3_WEB_EDITOR)
class DesignerView final : public CPluginView
#if SMTG_OS_LINUX
    , public Linux::ITimerHandler
#endif
{
public:
    explicit DesignerView(Controller& controller)
        // The controller's engine is a separate instance. Program memory belongs
        // to the processing component, even if this copy implements the interface.
        : controller_(&controller), parameters_(controller.engine, cell::designer::parameters, false
#if defined(CELL_DESIGNER_HAS_RULE_SOURCES)
            , {}, cell::designer::readOnlyParameters
#endif
        ) {
        const auto& descriptor = cell::plugin::descriptor();
        rect = {0, 0, int32(std::clamp(descriptor.editorWidth, 1u, 8192u)),
                       int32(std::clamp(descriptor.editorHeight, 1u, 8192u))};
    }

    ~DesignerView() override { detach(); }

    tresult PLUGIN_API isPlatformTypeSupported(FIDString type) override {
#if SMTG_OS_WINDOWS
        const auto platform = kPlatformTypeHWND;
#elif SMTG_OS_MACOS
        if (!pthread_main_np()) return kResultFalse;
        const auto platform = kPlatformTypeNSView;
#else
        const auto platform = kPlatformTypeX11EmbedWindowID;
#endif
        if (!type || std::strcmp(type, platform)) return kResultFalse;
        try { return cell::web::View::availability().available ? kResultTrue : kResultFalse; }
        catch (...) { return kResultFalse; }
    }

    tresult PLUGIN_API attached(void* parent, FIDString type) override {
        if (!parent || isAttached() || isPlatformTypeSupported(type) != kResultTrue) return kResultFalse;
#if SMTG_OS_LINUX
        FUnknownPtr<Linux::IRunLoop> loop(plugFrame);
        if (!loop) loop = controller_->getHostContext();
        if (!loop) return kResultFalse;
#endif
        try {
            cell::web::Options options;
#if SMTG_OS_WINDOWS
            const auto hostWindow = static_cast<HWND>(parent);
            if (GetWindowThreadProcessId(hostWindow, nullptr) != GetCurrentThreadId()) return kResultFalse;
            options.userDataDirectory = cell::web::win32::pluginProfile(L"VST3", "plugin-" CELL_VST3_WEB_PROFILE_ID);
            if (!createContainer(hostWindow)) return kResultFalse;
            options.parent = reinterpret_cast<std::uintptr_t>(window_);
            const auto startTimer = [&] { return window_ && SetTimer(window_, timerID, 33, nullptr) != 0; };
#elif SMTG_OS_MACOS
            if (!cell::web::cocoa::retainPluginModule()) return kResultFalse;
            options.parent = reinterpret_cast<std::uintptr_t>(parent);
            options.boundsUnits = cell::web::BoundsUnits::parentCoordinates;
            const auto startTimer = [&] {
                timer_ = cell::web::cocoa::startTimer(this,
                    [](CFRunLoopTimerRef, void* owner) { static_cast<DesignerView*>(owner)->update(); });
                return timer_ != nullptr;
            };
#else
            options.parent = reinterpret_cast<std::uintptr_t>(parent);
            const auto startTimer = [&] {
                if (loop->registerTimer(this, 33) != kResultOk) return false;
                runLoop_ = loop;
                return true;
            };
#endif
            options.bounds = {0, 0, rect.getWidth(), rect.getHeight()};
            const std::string_view page(reinterpret_cast<const char*>(cell::assets::vst3EditorPage),
                                        sizeof(cell::assets::vst3EditorPage));
            if (browser_.open(options) && browser_.loadHtml(page) && startTimer()) {
                active_ = true;
                designerSession_ = controller_->beginDesignerCommands();
                if constexpr (designerHasOutputMeter) controller_->designerMeterVisible(true);
                return CPluginView::attached(parent, type);
            }
        } catch (...) {}
        detach();
        return kResultFalse;
    }

    tresult PLUGIN_API removed() override {
        const auto result = CPluginView::removed();
        detach();
        return result;
    }

    tresult PLUGIN_API onFocus(TBool focused) override {
        if (!focused && active_) {
            parameters_.cancel();
            controller_->sendDesignerCommand(designerSession_, DesignerOperation::cancel);
            controller_->flushEdits();
        }
        return kResultTrue;
    }

    tresult PLUGIN_API onSize(ViewRect* size) override {
        if (!size) return kInvalidArgument;
        const auto width = int64(size->right) - size->left;
        const auto height = int64(size->bottom) - size->top;
        if (width < 1 || width > 8192 || height < 1 || height > 8192) return kResultFalse;
        try {
#if SMTG_OS_WINDOWS
            if (window_ && !SetWindowPos(window_, nullptr, 0, 0, int(width), int(height), SWP_NOACTIVATE | SWP_NOZORDER))
                return kResultFalse;
#endif
            if (active_) browser_.setBounds({0, 0, int(width), int(height)});
            return CPluginView::onSize(size);
        } catch (...) { return kResultFalse; }
    }

    tresult PLUGIN_API canResize() override { return kResultTrue; }

    tresult PLUGIN_API checkSizeConstraint(ViewRect* size) override {
        if (!size) return kInvalidArgument;
        const auto right = int64(size->left) + std::clamp<int64>(int64(size->right) - size->left, 1, 8192);
        const auto bottom = int64(size->top) + std::clamp<int64>(int64(size->bottom) - size->top, 1, 8192);
        if (right > INT32_MAX || bottom > INT32_MAX) return kResultFalse;
        size->right = int32(right);
        size->bottom = int32(bottom);
        return kResultTrue;
    }

#if SMTG_OS_LINUX
    void PLUGIN_API onTimer() override { update(); }
#endif

    OBJ_METHODS(DesignerView,CPluginView)
    DEFINE_INTERFACES
#if SMTG_OS_LINUX
        DEF_INTERFACE(Linux::ITimerHandler)
#endif
    END_DEFINE_INTERFACES(CPluginView)
    REFCOUNT_METHODS(CPluginView)

private:
    void update() {
        if (!active_) return;
        const IPtr<DesignerView> keepAlive(this);
        try {
            for (const auto& event : browser_.poll()) {
                if (event.kind == cell::web::Event::Kind::loaded) {
                    if (loaded_) controller_->sendDesignerCommand(designerSession_, DesignerOperation::cancel);
                    loaded_ = true;
                    lastParameters_.clear();
                    lastMeters_.clear();
                } else if (loaded_ && event.kind == cell::web::Event::Kind::message) {
                    try {
                        const auto message = cell::json::read(event.text, {designerSnapshotBytes, 4096, designerSnapshotBytes, 16});
                        const auto type = message["type"].text();
                        if (type == "action" || (type == "recall-program" && !parameters_.activeGestures()))
                            controller_->sendDesignerCommand(designerSession_, DesignerOperation::apply, event.text);
                        else if (type != "recall-program") parameters_.apply(event.text);
                    } catch (const cell::json::Error&) {}
                    lastParameters_.clear();
                } else if (event.kind == cell::web::Event::Kind::error) {
                    if (!loaded_) {
                        detach();
                        return;
                    }
                    parameters_.cancel();
                    controller_->sendDesignerCommand(designerSession_, DesignerOperation::cancel);
                }
            }
            if (loaded_) {
                controller_->refreshDesignerCommands(designerSession_);
                if (!active_) return;
                auto send = [&](std::string snapshot, std::string& previous) {
                    if (snapshot != previous && browser_.send(snapshot)) previous = std::move(snapshot);
                };
                send(controller_->designerSnapshot(designerSession_, parameters_.snapshot()), lastParameters_);
                if constexpr (designerHasOutputMeter) send(controller_->designerMeterSnapshot(), lastMeters_);
            }
        } catch (...) { parameters_.cancel(); }
        controller_->flushEdits();
    }

    void detach() {
        const bool wasActive = std::exchange(active_, false);
#if SMTG_OS_WINDOWS
        const auto window = std::exchange(window_, nullptr);
        if (window) {
            SetWindowLongPtrW(window, GWLP_USERDATA, 0);
            KillTimer(window, timerID);
        }
#elif SMTG_OS_MACOS
        cell::web::cocoa::stopTimer(timer_);
#else
        if (auto loop = std::move(runLoop_)) loop->unregisterTimer(this);
#endif
        browser_.close();
#if SMTG_OS_WINDOWS
        if (window) DestroyWindow(window);
#elif SMTG_OS_MACOS
        cell::web::View::readyToUnload();
#endif
        if (!wasActive) return;
        loaded_ = false;
        lastParameters_.clear();
        lastMeters_.clear();
        parameters_.cancel();
        controller_->sendDesignerCommand(designerSession_, DesignerOperation::close);
        try { controller_->flushDesignerCommands(designerSession_); } catch (...) {}
        if constexpr (designerHasOutputMeter) controller_->designerMeterVisible(false);
        controller_->flushEdits();
    }

#if SMTG_OS_WINDOWS
    static constexpr UINT_PTR timerID = 1;
    static constexpr wchar_t windowClass[] = L"CellFrameworkVst3DesignerEditor";

    static LRESULT CALLBACK dispatch(HWND window, UINT message, WPARAM wParam, LPARAM lParam) {
        if (message == WM_NCCREATE) {
            const auto* creation = reinterpret_cast<const CREATESTRUCTW*>(lParam);
            SetWindowLongPtrW(window, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(creation->lpCreateParams));
        }
        auto* view = reinterpret_cast<DesignerView*>(GetWindowLongPtrW(window, GWLP_USERDATA));
        if (view && message == WM_TIMER && wParam == timerID) {
            view->update();
            return 0;
        }
        if (view && message == WM_DESTROY && view->window_ == window) {
            // The host can destroy its parent HWND before calling removed().
            const IPtr<DesignerView> keepAlive(view);
            KillTimer(window, timerID);
            SetWindowLongPtrW(window, GWLP_USERDATA, 0);
            view->window_ = nullptr;
            view->removed();
        }
        return DefWindowProcW(window, message, wParam, lParam);
    }

    bool createContainer(HWND parent) {
        const auto module = cell::web::win32::pluginModule();
        if (!module) return false;
        static const auto registered = [module] {
            WNDCLASSW info{};
            info.hInstance = module;
            info.lpfnWndProc = dispatch;
            info.lpszClassName = windowClass;
            return RegisterClassW(&info);
        }();
        if (!registered) return false;
        window_ = CreateWindowExW(0, windowClass, L"", WS_CHILD | WS_VISIBLE | WS_CLIPCHILDREN | WS_CLIPSIBLINGS,
                                 0, 0, rect.getWidth(), rect.getHeight(), parent, nullptr, module, this);
        return window_ != nullptr;
    }
#endif

    IPtr<Controller> controller_;
    cell::web::ParameterBridge parameters_;
    cell::web::View browser_;
#if SMTG_OS_WINDOWS
    HWND window_{};
#elif SMTG_OS_MACOS
    CFRunLoopTimerRef timer_{};
#else
    IPtr<Linux::IRunLoop> runLoop_;
#endif
    bool active_{};
    int64 designerSession_{};
    bool loaded_{};
    std::string lastParameters_, lastMeters_;
};
#endif

IPlugView* PLUGIN_API Controller::createView(FIDString type){
#if CELL_HAS_UI
    if(type&&!std::strcmp(type,ViewType::kEditor)){if(analysis()&&views)return nullptr;try{return new View(*this);}catch(...){return nullptr;}}
#elif defined(CELL_VST3_WEB_EDITOR)
    if (type && !std::strcmp(type, ViewType::kEditor)) {
        try { return new DesignerView(*this); }
        catch (...) { return nullptr; }
    }
#else
    (void)type;
#endif
    return nullptr;
}
}

bool InitModule(){try{cell::jobs::resumeModulePools();return true;}catch(...){return false;}}
bool DeinitModule(){try{cell::jobs::quiesceModulePools();
#if CELL_HAS_UI && defined(_WIN32)
cell::ui::shutdownNativePlatformServices();
#endif
return true;}catch(...){return false;}}
BEGIN_FACTORY_DEF(cell::plugin::descriptor().vendor,"","")
DEF_CLASS2(INLINE_UID_FROM_FUID(processorId),PClassInfo::kManyInstances,kVstAudioEffectClass,cell::plugin::descriptor().name,CELL_VST3_CLASS_FLAGS,CELL_VST3_SUBCATEGORIES,cell::plugin::descriptor().version,kVstVersionString,Processor::create)
DEF_CLASS2(INLINE_UID_FROM_FUID(controllerId),PClassInfo::kManyInstances,kVstComponentControllerClass,CELL_VST3_CONTROLLER_NAME,0,"",cell::plugin::descriptor().version,kVstVersionString,Controller::create)
END_FACTORY
