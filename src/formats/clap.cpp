#include <cell/text.hpp>
#include <cell/jobs.hpp>
#include <cell/midi_delivery.hpp>
#include <mutex>
#include <cell/commands.hpp>
#include <cell/plugin.hpp>
#include <clap/clap.h>
#include <clap/ext/context-menu.h>
#if defined(CELL_CLAP_EXPOSE_PROGRAMS)
#include <clap/ext/preset-load.h>
#include <clap/factory/preset-discovery.h>
#include <cell/web_parameter_bridge.hpp>
#endif
#if CELL_HAS_UI
#include <cell/ui.hpp>
#endif
#if defined(CELL_CLAP_WEB_EDITOR)
#include <cell/webview.hpp>
#include <cell/web_meter.hpp>
#include <cell/web_parameter_bridge.hpp>
#include <condition_variable>
#include <thread>
#if defined(CELL_CLAP_EDITOR_FILE_TYPES)
#include <cell/file_dialog.hpp>
#include <cell/files.hpp>
#endif
#include "cell_clap_web_bindings.hpp"
#include "cell_clap_web_page.hpp"
#if defined(_WIN32)
#include "webview2_plugin.hpp"
#elif defined(__APPLE__)
#include "cocoa_web_editor.hpp"
#endif
#endif
#include <charconv>
#include <cstring>
#include <cstdlib>
#include <cstdio>
#include <functional>
#include <utility>

namespace {
using namespace cell;
void copy(char* dest,std::size_t capacity,const char* value) noexcept {
    if(capacity){std::strncpy(dest,value,capacity-1);dest[capacity-1]=0;}
}
std::span<const unsigned> portWidths() {
    struct Widths {
        std::array<unsigned,64> values{};
        std::size_t count{};
    };
    static const auto widths=[] {
        Widths result;
        const auto& descriptor=cell::plugin::descriptor();
        const auto add=[&](unsigned width) {
            if(width==0||width>64)return;
            const auto end=result.values.begin()+result.count;
            if(std::find(result.values.begin(),end,width)==end)
                result.values[result.count++]=width;
        };
        if(descriptor.mainBusLayouts.empty()) {
            add(descriptor.channels);
            if(descriptor.supportsMono)add(1);
        } else {
            for(const auto& layout:descriptor.mainBusLayouts)
                if(layout.input==layout.output&&channelCount(layout.output)==descriptor.channels)
                    add(descriptor.channels);
            for(const auto& layout:descriptor.mainBusLayouts)
                if(layout.input==layout.output)add(channelCount(layout.output));
        }
        return result;
    }();
    return {widths.values.data(),widths.count};
}
std::span<const std::uint8_t> surroundMap(unsigned width) noexcept {
    if(cell::plugin::descriptor().mainBusLayouts.empty())return {};
    // Keep the channel order used by the corresponding VST3 arrangements.
    static constexpr std::array<std::uint8_t,12> bedAndHeights{
        CLAP_SURROUND_FL,CLAP_SURROUND_FR,CLAP_SURROUND_FC,CLAP_SURROUND_LFE,
        CLAP_SURROUND_BL,CLAP_SURROUND_BR,CLAP_SURROUND_SL,CLAP_SURROUND_SR,
        CLAP_SURROUND_TFL,CLAP_SURROUND_TFR,CLAP_SURROUND_TBL,CLAP_SURROUND_TBR};
    static constexpr std::array<std::uint8_t,10> sideHeights{
        CLAP_SURROUND_FL,CLAP_SURROUND_FR,CLAP_SURROUND_FC,CLAP_SURROUND_LFE,
        CLAP_SURROUND_BL,CLAP_SURROUND_BR,CLAP_SURROUND_SL,CLAP_SURROUND_SR,
        CLAP_SURROUND_TSL,CLAP_SURROUND_TSR};
    if(width==channelCount(AudioLayout::surround51))return std::span(bedAndHeights).first(6);
    if(width==channelCount(AudioLayout::surround712))return sideHeights;
    if(width==channelCount(AudioLayout::surround714))return bedAndHeights;
    // The CLAP speaker enum has no front-wide roles for 9.1.6.
    return {};
}
bool supportsSurround() {
    return std::any_of(portWidths().begin(),portWidths().end(),
                       [](unsigned width){return !surroundMap(width).empty();});
}
const char* portType(unsigned width) noexcept {
    if(width==1)return CLAP_PORT_MONO;
    if(width==2)return CLAP_PORT_STEREO;
    return surroundMap(width).empty()?nullptr:CLAP_PORT_SURROUND;
}
const clap_plugin_descriptor_t& clapDescriptor() {
    const auto& info=cell::plugin::descriptor();
    static const auto features=[] {
        std::array<const char*,5> result{CLAP_PLUGIN_FEATURE_AUDIO_EFFECT};
        const auto widths=portWidths();
        unsigned index=1;
        if(std::find(widths.begin(),widths.end(),1)!=widths.end())result[index++]=CLAP_PLUGIN_FEATURE_MONO;
        if(std::find(widths.begin(),widths.end(),2)!=widths.end())result[index++]=CLAP_PLUGIN_FEATURE_STEREO;
        if(supportsSurround())result[index++]=CLAP_PLUGIN_FEATURE_SURROUND;
        return result;
    }();
    static const clap_plugin_descriptor_t descriptor{CLAP_VERSION,info.id,info.name,info.vendor,"","","",info.version,"Independent Cell framework development port",features.data()};
    return descriptor;
}

#if defined(CELL_CLAP_WEB_EDITOR)
struct WebEditor {
#if defined(CELL_DESIGNER_HAS_ACTIONS) || defined(CELL_DESIGNER_HAS_RULE_SOURCES)
    explicit WebEditor(Engine& engine) : parameters(cell::designer::bind(engine)) { prepareBridge(); }
#else
    explicit WebEditor(Engine& engine) : parameters(engine, cell::designer::parameters) { prepareBridge(); }
#endif
    web::ParameterBridge parameters;
    web::View view;
    std::uintptr_t parent{};
#if defined(__APPLE__)
    CFRunLoopTimerRef updateTimer{};
#else
    std::jthread fallbackTimer;
    std::atomic<bool> fallbackTick{};
#endif
    bool loaded{};
    std::string lastParameters, lastMeters;
#if defined(CELL_CLAP_EDITOR_FILE_TYPES)
    ui::FileDialog fileDialog;
    std::optional<web::EditorFileRequest> fileRequest;
    void pollFile() {
        const auto response = fileDialog.poll();
        if (!response || !fileRequest) return;
        auto request = std::exchange(fileRequest, std::nullopt);
        if (response->cancelled) {
            parameters.finishFile(true, {}, {});
            return;
        }
        if (!response->error.empty()) {
            parameters.finishFile(false, {}, {}, "The system file chooser did not complete");
            return;
        }
        auto extension = response->path.extension().string();
        for (auto& c : extension) if (c >= 'A' && c <= 'Z') c += 'a' - 'A';
        if (std::ranges::find(request->filters, extension) == request->filters.end()) {
            parameters.finishFile(false, {}, {}, "Selected file type is unavailable");
            return;
        }
        try {
            const auto filename = response->path.filename().u8string();
            const std::string name(reinterpret_cast<const char*>(filename.data()), filename.size());
            if (request->save) {
                files::atomicWrite(response->path, request->bytes);
                parameters.finishFile(false, name, {});
            } else {
                std::error_code error;
                const auto size = std::filesystem::file_size(response->path, error);
                if (!error && size > web::maxEditorFileBytes)
                    parameters.finishFile(false, {}, {}, "Selected file exceeds 45 KiB");
                else parameters.finishFile(false, name, files::readBounded(response->path, web::maxEditorFileBytes));
            }
        } catch (...) { parameters.finishFile(false, {}, {}, "The selected file could not be opened or saved"); }
    }
#endif
    void prepareBridge() {
        parameters.advertiseParameterDeltas();
#if defined(CELL_CLAP_EDITOR_FILE_TYPES)
        std::vector<std::string> extensions;
        auto text = std::string_view(CELL_CLAP_EDITOR_FILE_TYPES);
        while (!text.empty()) {
            const auto comma = text.find(',');
            extensions.emplace_back(text.substr(0, comma));
            if (comma == text.npos) break;
            text.remove_prefix(comma + 1);
        }
        parameters.setFileEndpoint(std::move(extensions), [this](web::EditorFileRequest request) {
            if (!fileDialog.open({parent, request.save ? "Save preset file" : "Open preset file",
                                 {}, request.filters, request.save, false})) return false;
            fileRequest = std::move(request);
            return true;
        });
#endif
    }
};
#endif

struct Instance {
    clap_plugin_t plugin{};
    const clap_host_t* host{};
    const clap_host_params_t* hostParams{};
    const clap_host_context_menu_t* hostContextMenu{};
    const clap_host_state_t* hostState{};const clap_host_latency_t* hostLatency{};std::uint32_t reportedLatency{};std::atomic<bool> restartRequested{};
    const clap_host_tail_t* hostTail{};
    std::atomic<std::uint32_t> reportedTail{};
    Engine engine{cell::plugin::descriptor(),cell::plugin::createProcessor()};
    const ParameterPresentationSource* presentationSource{};
    // Audio/flush see a fixed domain while activated. Main-thread metadata may
    // refresh independently without reallocating or racing the audio reader.
    std::vector<ParameterPresentation> wirePresentation, editorPresentation;
    std::optional<std::uint64_t> presentationRevision;
    std::atomic<bool> presentationCheckPending{};
    bool presentationRestartPending{};
    bool active{};std::atomic<bool> processing{};
    std::uint32_t channels=portWidths().front();
    std::array<ParameterEvent,4096> events{},eventScratch{};std::array<MidiEvent,2048> midi{},midiScratch{};
    midi::Delivery midiDelivery;
    ParameterEdit pendingEdit{};
    bool hasPendingEdit{};
    double sampleRate=48000;
    std::uint32_t maxFrames{};
    std::vector<float> mainInputScratch,sidechainScratch;
    bool offline=false;
#if CELL_HAS_UI
    std::unique_ptr<ui::Editor> editor;
    std::unique_ptr<ui::Window> window;
    double scale=1;
#endif
#if CELL_HAS_UI || defined(CELL_CLAP_WEB_EDITOR)
    const clap_host_gui_t* hostGui{};
    const clap_host_timer_support_t* timers{};
    clap_id timer=CLAP_INVALID_ID;
    std::uint32_t guiWidth=cell::plugin::descriptor().editorWidth,guiHeight=cell::plugin::descriptor().editorHeight;
#endif
#if defined(CELL_CLAP_WEB_EDITOR)
    std::unique_ptr<WebEditor> webEditor;
    const clap_host_log_t* hostLog{};
    web::OutputMeter editorMeter, editorInputMeter;
    const GainReductionSource* gainReduction{};
    std::atomic<bool> editorMetersEnabled{};
    std::atomic<float> reductionDb{};
    void setEditorMeters(bool enabled) noexcept {
        editorMeter.setEnabled(enabled);
        editorInputMeter.setEnabled(enabled);
        editorMetersEnabled.store(enabled, std::memory_order_relaxed);
    }
    void clearEditorMeters() noexcept {
        editorMeter.clear();
        editorInputMeter.clear();
        reductionDb.store(0, std::memory_order_relaxed);
    }
#endif
    static Instance& get(const clap_plugin_t* p) noexcept {return *static_cast<Instance*>(p->plugin_data);}
    double hostValue(ParamId id, double storageValue) const noexcept {
        if (wirePresentation.empty()) return storageValue;
        const auto index = engine.parameters().indexOf(id);
        if (index >= wirePresentation.size()) return storageValue;
        return mapParameterValue(storageValue, engine.parameters().descriptors()[index], wirePresentation[index].field);
    }
    void outputEdits(const clap_output_events_t* out) noexcept {
        if(!out||!out->try_push)return;
        ParameterEdit edit;
        while(hasPendingEdit||engine.pollEdit(edit)) {
            if(hasPendingEdit)edit=pendingEdit;
            bool accepted=false;
            if(edit.type==EditType::value) {
                clap_event_param_value_t event{};event.header={sizeof(event),0,CLAP_CORE_EVENT_SPACE_ID,CLAP_EVENT_PARAM_VALUE,CLAP_EVENT_IS_LIVE};
                event.param_id=edit.id;event.note_id=-1;event.port_index=event.channel=event.key=-1;event.value=hostValue(edit.id,edit.value);
                accepted=out->try_push(out,&event.header);
            } else {
                clap_event_param_gesture_t event{};event.header={sizeof(event),0,CLAP_CORE_EVENT_SPACE_ID,
                    std::uint16_t(edit.type==EditType::begin?CLAP_EVENT_PARAM_GESTURE_BEGIN:CLAP_EVENT_PARAM_GESTURE_END),CLAP_EVENT_IS_LIVE};
                event.param_id=edit.id;accepted=out->try_push(out,&event.header);
            }
            if(!accepted){pendingEdit=edit;hasPendingEdit=true;return;}
            hasPendingEdit=false;
        }
    }
};

void refreshPresentation(Instance& instance, bool restored = false, bool notify = true) {
    const auto* source = instance.presentationSource;
    auto flags = restored ? clap_param_rescan_flags(CLAP_PARAM_RESCAN_VALUES | CLAP_PARAM_RESCAN_TEXT |
                                                    CLAP_PARAM_RESCAN_INFO) : 0u;
    if (source) {
        const auto revision = source->parameterPresentationRevision();
        if (instance.presentationRevision != revision || (!instance.active && instance.presentationRestartPending)) {
            std::vector<ParameterPresentation> next;
            next.reserve(instance.engine.parameters().descriptors().size());
            for (const auto& field : instance.engine.parameters().descriptors())
                next.push_back(resolveParameterPresentation(field, source));
            bool critical = false;
            for (std::size_t n = 0; n < instance.wirePresentation.size(); ++n) {
                const auto& wire = instance.wirePresentation[n];
                auto& current = next[n];
                const bool changed = !sameParameterDomain(wire.field, current.field) || wire.readOnly != current.readOnly ||
                    wire.field.automatable != current.field.automatable ||
                    wire.field.choices.empty() != current.field.choices.empty();
                critical |= changed;
                if (changed && instance.active) {
                    // Keep the old range, choices, step and editability coherent
                    // until the host reactivates, but publish name and visibility.
                    auto name = std::move(current.field.name);
                    const auto hidden = current.hidden;
                    current = instance.editorPresentation[n];
                    current.field.name = std::move(name);
                    current.hidden = hidden;
                }
            }
            flags |= CLAP_PARAM_RESCAN_VALUES | CLAP_PARAM_RESCAN_TEXT | CLAP_PARAM_RESCAN_INFO;
            if (!instance.active) {
                instance.wirePresentation = next;
                instance.presentationRestartPending = false;
                if (critical) flags |= CLAP_PARAM_RESCAN_ALL;
            }
            instance.editorPresentation = std::move(next);
            instance.presentationRevision = revision;
            if (instance.active && critical && !instance.presentationRestartPending) {
                instance.presentationRestartPending = true;
                if (instance.host->request_restart) instance.host->request_restart(instance.host);
            }
        }
    }
    if (notify && flags && instance.hostParams && instance.hostParams->rescan)
        instance.hostParams->rescan(instance.host, flags);
}

void requestPresentationCheck(Instance& instance) noexcept {
    if (instance.presentationSource && instance.host->request_callback &&
        !instance.presentationCheckPending.exchange(true, std::memory_order_acq_rel))
        instance.host->request_callback(instance.host);
}

#if defined(CELL_CLAP_WEB_EDITOR)
void updateWebEditor(Instance&);
#endif

void CLAP_ABI onMainThread(const clap_plugin_t* plugin) {
    auto& instance = Instance::get(plugin);
    instance.presentationCheckPending.store(false, std::memory_order_release);
    try { refreshPresentation(instance); } catch (...) {}
#if defined(CELL_CLAP_WEB_EDITOR) && !defined(__APPLE__)
    if (instance.webEditor && instance.webEditor->fallbackTimer.joinable() &&
        instance.webEditor->fallbackTick.exchange(false, std::memory_order_acq_rel))
        updateWebEditor(instance);
#endif
}

#if defined(CELL_CLAP_WEB_EDITOR)
bool webEditorRunning(const Instance& instance) {
#if defined(__APPLE__)
    return instance.webEditor && instance.webEditor->updateTimer;
#else
    return instance.webEditor && (instance.timer != CLAP_INVALID_ID || instance.webEditor->fallbackTimer.joinable());
#endif
}

void closeWebView(Instance& instance, bool flush) {
    if (!instance.webEditor) return;
    auto& editor = *instance.webEditor;
    const auto gestures = editor.parameters.activeGestures() + editor.parameters.activeActions();
    editor.parameters.cancel();
#if defined(CELL_CLAP_EDITOR_FILE_TYPES)
    editor.fileDialog.cancel();
    editor.fileRequest.reset();
#endif
    instance.setEditorMeters(false);
#if defined(__APPLE__)
    web::cocoa::stopTimer(editor.updateTimer);
#else
    if (instance.timer != CLAP_INVALID_ID) {
        const auto timer = std::exchange(instance.timer, CLAP_INVALID_ID);
        instance.timers->unregister_timer(instance.host, timer);
    }
    if (editor.fallbackTimer.joinable()) {
        editor.fallbackTimer.request_stop();
        editor.fallbackTimer.join();
        editor.fallbackTick.store(false, std::memory_order_relaxed);
    }
#endif
    editor.view.close();
#if defined(__APPLE__)
    // Dispose the runtime delegate class when WebKit has released all delegates.
    web::View::readyToUnload();
#endif
    editor.loaded = false;
    editor.parameters.resetPage();
    editor.lastParameters.clear();
    editor.lastMeters.clear();
    if (flush && gestures && instance.hostParams && instance.hostParams->request_flush)
        instance.hostParams->request_flush(instance.host);
}
#endif

// CLAP permits tail queries on the main thread during audio processing.
// Publish a snapshot from the thread that owns the processor at this point.
void publishTail(Instance& instance, bool audioThread) noexcept {
    const auto current=instance.engine.processor().tail();
    const auto previous=instance.reportedTail.exchange(current,std::memory_order_relaxed);
    if(audioThread&&current!=previous&&instance.hostTail&&instance.hostTail->changed)
        instance.hostTail->changed(instance.host);
}

bool CLAP_ABI init(const clap_plugin_t* p) {
    auto& i=Instance::get(p);
    i.hostParams=static_cast<const clap_host_params_t*>(i.host->get_extension(i.host,CLAP_EXT_PARAMS));
    i.hostContextMenu=static_cast<const clap_host_context_menu_t*>(i.host->get_extension(i.host,CLAP_EXT_CONTEXT_MENU));
    if(!i.hostContextMenu)i.hostContextMenu=static_cast<const clap_host_context_menu_t*>(i.host->get_extension(i.host,CLAP_EXT_CONTEXT_MENU_COMPAT));
    i.hostState=static_cast<const clap_host_state_t*>(i.host->get_extension(i.host,CLAP_EXT_STATE));
    i.hostLatency=static_cast<const clap_host_latency_t*>(i.host->get_extension(i.host,CLAP_EXT_LATENCY));
    i.hostTail=static_cast<const clap_host_tail_t*>(i.host->get_extension(i.host,CLAP_EXT_TAIL));
    i.presentationSource = dynamic_cast<const ParameterPresentationSource*>(&i.engine.processor());
    try { refreshPresentation(i, false, false); } catch (...) { return false; }
    publishTail(i,false);
#if CELL_HAS_UI || defined(CELL_CLAP_WEB_EDITOR)
    i.timers=static_cast<const clap_host_timer_support_t*>(i.host->get_extension(i.host,CLAP_EXT_TIMER_SUPPORT));
    i.hostGui=static_cast<const clap_host_gui_t*>(i.host->get_extension(i.host,CLAP_EXT_GUI));
#endif
#if defined(CELL_CLAP_WEB_EDITOR)
    i.hostLog=static_cast<const clap_host_log_t*>(i.host->get_extension(i.host,CLAP_EXT_LOG));
    i.gainReduction = dynamic_cast<const GainReductionSource*>(&i.engine.processor());
#endif
    return true;
}
void CLAP_ABI destroy(const clap_plugin_t* p) {
    auto& i=Instance::get(p);
#if CELL_HAS_UI
    if(i.timer!=CLAP_INVALID_ID&&i.timers)i.timers->unregister_timer(i.host,i.timer);
#endif
#if defined(CELL_CLAP_WEB_EDITOR)
    closeWebView(i, false);
#endif
    delete &i;
}
bool CLAP_ABI activate(const clap_plugin_t* p,double sampleRate,std::uint32_t minFrames,std::uint32_t maxFrames) {
    auto& i=Instance::get(p);if(i.active||minFrames==0||minFrames>maxFrames)return false;
    try {
        i.engine.prepare(sampleRate,maxFrames,i.channels);
        i.sampleRate=sampleRate;
        if(i.channels>1)i.mainInputScratch.resize(std::size_t(maxFrames)*(i.channels-1));
        if(cell::plugin::descriptor().supportsSidechain)
            i.sidechainScratch.resize(std::size_t(maxFrames)*2);
        i.maxFrames=maxFrames;
        const auto preparedLatency=i.engine.processor().latency();
        const bool changed=preparedLatency!=i.reportedLatency;
        i.reportedLatency=preparedLatency;
        publishTail(i,false);
        if(changed&&i.hostLatency&&i.hostLatency->changed)i.hostLatency->changed(i.host);
        i.restartRequested=false;
        i.active=true;
        return true;
    } catch(...) { return false; }
}
void CLAP_ABI deactivate(const clap_plugin_t* p) {
    auto& i = Instance::get(p);
    i.processing = false;
    i.active = false;
    try { refreshPresentation(i); } catch (...) {}
}
bool CLAP_ABI start(const clap_plugin_t* p) {auto& i=Instance::get(p);if(!i.active||i.processing)return false;i.processing=true;return true;}
void CLAP_ABI stop(const clap_plugin_t* p) {
    auto& i=Instance::get(p);
    i.processing=false;
    i.midiDelivery.reset();
#if defined(CELL_CLAP_WEB_EDITOR)
    i.clearEditorMeters();
#endif
}
void CLAP_ABI reset(const clap_plugin_t* p) {
    auto& i=Instance::get(p);
    i.engine.reset();
    i.midiDelivery.reset();
#if defined(CELL_CLAP_WEB_EDITOR)
    i.clearEditorMeters();
#endif
    publishTail(i,true);
}

std::optional<ParameterEdit> hostParameterEdit(const Instance& instance, const clap_event_header_t* h) noexcept {
    if (!h || h->space_id != CLAP_CORE_EVENT_SPACE_ID) return {};
    ParameterEdit edit;
    if (h->type == CLAP_EVENT_PARAM_VALUE && h->size >= sizeof(clap_event_param_value_t)) {
        const auto& event = *reinterpret_cast<const clap_event_param_value_t*>(h);
        if (event.note_id != -1 || event.channel != -1 || event.key != -1 ||
            !std::isfinite(event.value)) return {};
        edit = {EditType::value, event.param_id, event.value};
    } else if ((h->type == CLAP_EVENT_PARAM_GESTURE_BEGIN || h->type == CLAP_EVENT_PARAM_GESTURE_END) &&
               h->size >= sizeof(clap_event_param_gesture_t)) {
        edit = {h->type == CLAP_EVENT_PARAM_GESTURE_BEGIN ? EditType::begin : EditType::end,
                reinterpret_cast<const clap_event_param_gesture_t*>(h)->param_id, 0};
    } else return {};
    const auto fields = instance.engine.parameters().descriptors();
    const auto index = instance.engine.parameters().indexOf(edit.id);
    if (index >= fields.size()) return {};
    if (!instance.wirePresentation.empty()) {
        const auto& presented = instance.wirePresentation[index];
        if (presented.readOnly) return {};
        if (edit.type == EditType::value) edit.value = mapParameterValue(edit.value, presented.field, fields[index]);
    }
    return edit;
}
bool overlapsAudio(const float* source,const float* destination,unsigned frames) noexcept {
    const std::less<const float*> before;
    return before(source,destination+frames)&&before(destination,source+frames);
}

void copyMainInput(Instance& instance,const clap_process_t& block,float* const* output) noexcept {
    const auto frames=block.frames_count;
    if(!frames)return;
    const auto channels=instance.channels;
    std::array<const float*,64> inputs{};
    if(block.audio_inputs_count&&block.audio_inputs&&block.audio_inputs[0].data32) {
        const auto& bus=block.audio_inputs[0];
        std::copy_n(bus.data32,std::min(channels,bus.channel_count),inputs.begin());
    }

    // Save inputs that an earlier destination could overwrite. The first
    // channel never needs scratch because its own copy uses memmove.
    for(unsigned c=1;c<channels;++c) {
        if(!inputs[c])continue;
        const auto wouldOverwrite=[&](const float* destination) {
            return overlapsAudio(inputs[c],destination,frames);
        };
        if(std::any_of(output,output+c,wouldOverwrite)) {
            auto* scratch=instance.mainInputScratch.data()+std::size_t(c-1)*instance.maxFrames;
            std::copy_n(inputs[c],frames,scratch);
            inputs[c]=scratch;
        }
    }
    for(unsigned c=0;c<channels;++c) {
        if(!inputs[c])std::fill_n(output[c],frames,0.f);
        else if(inputs[c]!=output[c])std::memmove(output[c],inputs[c],std::size_t(frames)*sizeof(float));
    }
}
AudioBlock<float> sidechainInput(Instance& instance,const clap_process_t& block,
                                float* const* output,std::array<float*,2>& channels) noexcept {
    if(instance.sidechainScratch.empty()||block.audio_inputs_count<2||!block.audio_inputs)return {};
    const auto& bus=block.audio_inputs[1];
    if(!bus.data32||bus.channel_count==0||bus.channel_count>channels.size())return {};
    for(unsigned c=0;c<bus.channel_count;++c)
        if(!bus.data32[c])return {};

    const auto frames=block.frames_count;
    for(unsigned c=0;c<bus.channel_count;++c) {
        const auto* source=bus.data32[c];
        channels[c]=bus.data32[c];
        const auto overlaps=[&](const float* destination) {
            return overlapsAudio(source,destination,frames);
        };
        if(frames&&std::any_of(output,output+instance.channels,overlaps)) {
            auto* scratch=instance.sidechainScratch.data()+std::size_t(c)*instance.maxFrames;
            std::copy_n(source,frames,scratch);
            channels[c]=scratch;
        }
    }
    return {channels.data(),bus.channel_count,frames,0};
}
struct MidiOutput final:midi::Output {
    const clap_output_events_t* destination{};
    bool push(MidiEvent message) noexcept override {
        if (message.isUmp()) {
            if (!cell::plugin::descriptor().supportsUmp || !midi::validUmp(message)) return false;
            clap_event_midi2_t event{};
            event.header = {sizeof(event), message.frame, CLAP_CORE_EVENT_SPACE_ID, CLAP_EVENT_MIDI2, 0};
            std::copy_n(message.ump.begin(), message.umpCount, event.data);
            return destination->try_push(destination, &event.header);
        }
        if (message.isSysEx()) {
            if (!cell::plugin::descriptor().supportsSysEx) return false;
            clap_event_midi_sysex_t event{};
            event.header = {sizeof(event), message.frame, CLAP_CORE_EVENT_SPACE_ID, CLAP_EVENT_MIDI_SYSEX, 0};
            event.buffer = message.sysEx.data();
            event.size = std::uint32_t(message.sysEx.size());
            return destination->try_push(destination, &event.header);
        }
        clap_event_midi_t event{};
        event.header={sizeof(event),message.frame,CLAP_CORE_EVENT_SPACE_ID,CLAP_EVENT_MIDI,0};
        event.port_index=0;
        event.data[0]=message.data[0];
        event.data[1]=message.data[1];
        event.data[2]=message.size==3?message.data[2]:0;
        return destination->try_push(destination,&event.header);
    }
};
clap_process_status CLAP_ABI process(const clap_plugin_t* p,const clap_process_t* block) {
    auto& i=Instance::get(p);
    if(!i.processing||!block||block->frames_count>i.maxFrames||block->audio_outputs_count!=1||!block->audio_outputs||block->audio_outputs[0].channel_count!=i.channels||!block->audio_outputs[0].data32)
        return CLAP_PROCESS_ERROR;
    auto** output=block->audio_outputs[0].data32;
    for(unsigned c=0;c<i.channels;++c) {
        if(!output[c])return CLAP_PROCESS_ERROR;
        if(block->frames_count&&std::any_of(output,output+c,[&](const float* earlier) {
            return overlapsAudio(output[c],earlier,block->frames_count);
        }))return CLAP_PROCESS_ERROR;
    }
    Transport transport;transport.offline=i.offline;transport.tempoValid=false;
    if(const auto* t=block->transport) {
        transport.playing=(t->flags&CLAP_TRANSPORT_IS_PLAYING)!=0;
        transport.recording=(t->flags&CLAP_TRANSPORT_IS_RECORDING)!=0;
        transport.looping=(t->flags&CLAP_TRANSPORT_IS_LOOP_ACTIVE)!=0;
        if((t->flags&CLAP_TRANSPORT_HAS_TEMPO)&&std::isfinite(t->tempo)&&t->tempo>0){transport.tempo=t->tempo;transport.tempoValid=true;}
        if(t->flags&CLAP_TRANSPORT_HAS_BEATS_TIMELINE){transport.beatPosition=double(t->song_pos_beats)/CLAP_BEATTIME_FACTOR;transport.beatPositionValid=true;}
        if(t->flags&CLAP_TRANSPORT_HAS_SECONDS_TIMELINE){transport.samplePosition=std::int64_t(double(t->song_pos_seconds)/CLAP_SECTIME_FACTOR*i.sampleRate);transport.samplePositionValid=true;}
        if(t->flags&CLAP_TRANSPORT_HAS_TIME_SIGNATURE){transport.numerator=t->tsig_num;transport.denominator=t->tsig_denom;}
    }
    AudioBlock<float> audio{output,i.channels,block->frames_count,0};
    midi::SysExBudget sysEx;
    const auto& descriptor = cell::plugin::descriptor();
    std::size_t eventCount=0,midiCount=0;const auto count=block->in_events?block->in_events->size(block->in_events):0;
    for(std::uint32_t n=0;n<count;++n){const auto* h=block->in_events->get(block->in_events,n);if(!h||h->time>block->frames_count)continue;
        if (const auto edit = hostParameterEdit(i, h)) {
            if (eventCount == i.events.size()) return CLAP_PROCESS_ERROR;
            i.events[eventCount++] = {h->time, edit->id, edit->value, edit->type};
        }
        else if(cell::plugin::descriptor().acceptsMidi&&h->space_id==CLAP_CORE_EVENT_SPACE_ID&&h->type==CLAP_EVENT_MIDI&&h->size>=sizeof(clap_event_midi_t)&&h->time<block->frames_count){auto* e=reinterpret_cast<const clap_event_midi_t*>(h);if(e->port_index!=0||e->data[0]<0x80||e->data[0]>=0xf0)continue;if(midiCount==i.midi.size())return CLAP_PROCESS_ERROR;i.midi[midiCount++]={h->time,{e->data[0],e->data[1],e->data[2]},3};}
        else if (descriptor.acceptsMidi && descriptor.supportsSysEx && h->space_id == CLAP_CORE_EVENT_SPACE_ID &&
                 h->type == CLAP_EVENT_MIDI_SYSEX && h->size >= sizeof(clap_event_midi_sysex_t) && h->time < block->frames_count) {
            const auto& event = *reinterpret_cast<const clap_event_midi_sysex_t*>(h);
            if (event.port_index != 0 || !event.buffer || !sysEx.admit({event.buffer, event.size})) continue;
            if (midiCount == i.midi.size()) return CLAP_PROCESS_ERROR;
            i.midi[midiCount++] = MidiEvent::systemExclusive(h->time, {event.buffer, event.size});
        }
        else if (descriptor.acceptsMidi && descriptor.supportsUmp && h->space_id == CLAP_CORE_EVENT_SPACE_ID &&
                 h->type == CLAP_EVENT_MIDI2 && h->size >= sizeof(clap_event_midi2_t) && h->time < block->frames_count) {
            const auto& event = *reinterpret_cast<const clap_event_midi2_t*>(h);
            const auto words = umpWordCount(event.data[0]);
            if (event.port_index != 0 || !words) continue;
            if (midiCount == i.midi.size()) return CLAP_PROCESS_ERROR;
            i.midi[midiCount++] = MidiEvent::universalPacket(h->time, {event.data, words});
        }
    }
    stableEventSort({i.events.data(),eventCount},i.eventScratch);stableFrameSort<MidiEvent>({i.midi.data(),midiCount},i.midiScratch);
    std::array<float*,2> sidechainChannels{};
    const auto side=sidechainInput(i,*block,output,sidechainChannels);
    copyMainInput(i,*block,output);
#if defined(CELL_CLAP_WEB_EDITOR)
    i.editorInputMeter.publish(audio);
#endif
    i.outputEdits(block->out_events);
    MidiOutput midiOutput;
    const bool sendsMidi=cell::plugin::descriptor().producesMidi&&block->frames_count;
    if(sendsMidi) {
        if(block->out_events&&block->out_events->try_push)midiOutput.destination=block->out_events;
        i.midiDelivery.begin(midiOutput.destination?&midiOutput:nullptr,block->frames_count);
    }
    i.engine.process(audio,{i.events.data(),eventCount},transport,{i.midi.data(),midiCount},side,
                     sendsMidi?&i.midiDelivery:nullptr);
#if defined(CELL_CLAP_WEB_EDITOR)
    i.editorMeter.publish(audio);
    if (i.gainReduction && i.editorMetersEnabled.load(std::memory_order_relaxed))
        i.reductionDb.store(i.gainReduction->gainReductionDb(), std::memory_order_relaxed);
#endif
    publishTail(i,true);
    if (eventCount || midiCount) requestPresentationCheck(i);
    if(i.engine.processor().latency()!=i.reportedLatency&&!i.restartRequested.exchange(true)&&i.host->request_restart)i.host->request_restart(i.host);
    // CLAP requires nondecreasing timestamps. Any edits queued during a
    // MIDI-output block can be sent at the start of the next block.
    if(!sendsMidi)i.outputEdits(block->out_events);
    block->audio_outputs[0].constant_mask=0;
    return CLAP_PROCESS_CONTINUE;
}

std::uint32_t audioPortCount(bool input) {
    return input&&cell::plugin::descriptor().supportsSidechain?2u:1u;
}
bool audioPortInfo(unsigned width,std::uint32_t index,bool input,clap_audio_port_info_t* info) {
    if(!info||index>=audioPortCount(input))return false;
    *info={};
    if(index) {
        info->id=2;
        copy(info->name,sizeof(info->name),"Sidechain");
        info->channel_count=std::min(2u,width);
        info->port_type=portType(info->channel_count);
        info->in_place_pair=CLAP_INVALID_ID;
    } else {
        info->id=input?0:1;
        copy(info->name,sizeof(info->name),input?"Input":"Output");
        info->flags=CLAP_AUDIO_PORT_IS_MAIN;
        info->channel_count=width;
        info->port_type=portType(width);
        info->in_place_pair=input?1:0;
    }
    return true;
}
const clap_plugin_audio_ports_t ports{
    [](const clap_plugin_t*,bool input)->std::uint32_t{return audioPortCount(input);},
    [](const clap_plugin_t* p,std::uint32_t index,bool input,clap_audio_port_info_t* info)->bool{
        return audioPortInfo(Instance::get(p).channels,index,input,info);
    }
};
const clap_plugin_surround_t surround{
    [](const clap_plugin_t*,std::uint64_t mask)->bool{
        for(const auto width:portWidths()) {
            const auto map=surroundMap(width);
            if(map.empty())continue;
            std::uint64_t supported{};
            for(const auto speaker:map)supported|=std::uint64_t{1}<<speaker;
            if(mask==supported)return true;
        }
        return false;
    },
    [](const clap_plugin_t* p,bool,std::uint32_t index,std::uint8_t* map,std::uint32_t capacity)->std::uint32_t{
        const auto speakers=surroundMap(Instance::get(p).channels);
        if(index!=0||!map||speakers.empty()||capacity<speakers.size())return 0;
        std::copy(speakers.begin(),speakers.end(),map);
        return static_cast<std::uint32_t>(speakers.size());
    }
};
bool hasNotePort(bool input) {
    const auto& descriptor=cell::plugin::descriptor();
    return input?descriptor.acceptsMidi:descriptor.producesMidi;
}
const clap_plugin_note_ports_t notePorts{
    [](const clap_plugin_t*,bool input)->std::uint32_t{return hasNotePort(input)?1u:0u;},
    [](const clap_plugin_t*,std::uint32_t index,bool input,clap_note_port_info_t* info)->bool{
        if(index||!info||!hasNotePort(input))return false;
        *info={};
        info->id=0;
        info->supported_dialects=info->preferred_dialect=CLAP_NOTE_DIALECT_MIDI;
        if (cell::plugin::descriptor().supportsUmp) {
            info->supported_dialects |= CLAP_NOTE_DIALECT_MIDI2;
            info->preferred_dialect = CLAP_NOTE_DIALECT_MIDI2;
        }
        copy(info->name,sizeof(info->name),input?"MIDI Input":"MIDI Output");
        return true;
    }
};
const clap_plugin_audio_ports_config_t portConfigurations{
    [](const clap_plugin_t*)->std::uint32_t{return static_cast<std::uint32_t>(portWidths().size());},
    [](const clap_plugin_t*,std::uint32_t index,clap_audio_ports_config_t* info)->bool{
        const auto widths=portWidths();
        if(!info||index>=widths.size())return false;
        const auto width=widths[index];
        *info={};info->id=index;
        if(width<=2)copy(info->name,sizeof(info->name),width==1?"Mono":"Stereo");
        else std::snprintf(info->name,sizeof(info->name),"%u channels",width);
        info->input_port_count=audioPortCount(true);info->output_port_count=audioPortCount(false);
        info->has_main_input=info->has_main_output=true;info->main_input_channel_count=info->main_output_channel_count=width;
        info->main_input_port_type=info->main_output_port_type=portType(width);return true;
    },
    [](const clap_plugin_t* p,clap_id id)->bool{
        auto& i=Instance::get(p);
        const auto widths=portWidths();
        if(i.active||id>=widths.size())return false;
        i.channels=widths[id];
        return true;
    }
};
const clap_plugin_audio_ports_config_info_t portConfigurationInfo{
    [](const clap_plugin_t* p)->clap_id{
        const auto widths=portWidths();
        const auto selected=std::find(widths.begin(),widths.end(),Instance::get(p).channels);
        return selected==widths.end()?CLAP_INVALID_ID:static_cast<clap_id>(selected-widths.begin());
    },
    [](const clap_plugin_t*,clap_id id,std::uint32_t index,bool input,clap_audio_port_info_t* info)->bool{
        const auto widths=portWidths();
        return id<widths.size()&&audioPortInfo(widths[id],index,input,info);
    }
};
unsigned requestedPortWidth(const Instance& instance,
                            const clap_audio_port_configuration_request_t* requests,
                            std::uint32_t count) {
    if(instance.active||!requests||count==0||count>audioPortCount(true)+audioPortCount(false))return 0;

    // Index by port ID: main input, main output, then optional sidechain.
    std::array<const clap_audio_port_configuration_request_t*,3> byPort{};
    for(std::uint32_t n=0;n<count;++n) {
        const auto& request=requests[n];
        if(request.port_index>=audioPortCount(request.is_input))return 0;
        const auto slot=request.port_index?2u:request.is_input?0u:1u;
        if(byPort[slot])return 0;
        byPort[slot]=&request;
    }

    for(const auto width:portWidths()) {
        bool matches=true;
        for(unsigned slot=0;slot<audioPortCount(true)+audioPortCount(false);++slot) {
            const bool input=slot!=1;
            const unsigned index=slot==2?1u:0u;
            clap_audio_port_info_t port{};
            audioPortInfo(width,index,input,&port);
            const auto* request=byPort[slot];
            if(!request) {
                clap_audio_port_info_t existing{};
                audioPortInfo(instance.channels,index,input,&existing);
                matches=port.channel_count==existing.channel_count;
            } else {
                const std::string_view type=request->port_type?request->port_type:"";
                matches=request->channel_count==port.channel_count&&type==(port.port_type?port.port_type:"");
                if(matches&&type==CLAP_PORT_SURROUND) {
                    const auto speakers=surroundMap(width);
                    const auto* map=static_cast<const std::uint8_t*>(request->port_details);
                    matches=map&&std::equal(speakers.begin(),speakers.end(),map);
                }
            }
            if(!matches)break;
        }
        if(matches)return width;
    }
    return 0;
}
const clap_plugin_configurable_audio_ports_t configurablePorts{
    [](const clap_plugin_t* p,const clap_audio_port_configuration_request_t* requests,std::uint32_t count)->bool{
        return requestedPortWidth(Instance::get(p),requests,count)!=0;
    },
    [](const clap_plugin_t* p,const clap_audio_port_configuration_request_t* requests,std::uint32_t count)->bool{
        auto& instance=Instance::get(p);
        const auto width=requestedPortWidth(instance,requests,count);
        if(!width)return false;
        instance.channels=width;
        return true;
    }
};
const Parameter& hostField(const Instance& instance, std::size_t index) noexcept {
    return instance.editorPresentation.empty() ? instance.engine.parameters().descriptors()[index] :
                                                instance.editorPresentation[index].field;
}

const clap_plugin_params_t params{
    [](const clap_plugin_t* p)->std::uint32_t{return std::uint32_t(Instance::get(p).engine.parameters().descriptors().size());},
    [](const clap_plugin_t* p,std::uint32_t index,clap_param_info_t* info)->bool{
        const auto& instance = Instance::get(p);
        const auto all = instance.engine.parameters().descriptors();
        if (!info || index >= all.size()) return false;
        const auto& param = hostField(instance, index);
        const auto* presentation = instance.editorPresentation.empty() ? nullptr : &instance.editorPresentation[index];
        const bool readOnly = presentation && presentation->readOnly;
        *info={};info->id=param.id;info->cookie=const_cast<Parameter*>(&all[index]);
        info->flags=param.automatable&&!readOnly?CLAP_PARAM_IS_AUTOMATABLE|CLAP_PARAM_REQUIRES_PROCESS:0;
        if (readOnly) info->flags |= CLAP_PARAM_IS_READONLY;
        if (presentation && presentation->hidden) info->flags |= CLAP_PARAM_IS_HIDDEN;
        if(param.step>=1)info->flags|=CLAP_PARAM_IS_STEPPED;
        if(!param.choices.empty())info->flags|=CLAP_PARAM_IS_ENUM;
        if(param.bypass)info->flags|=CLAP_PARAM_IS_BYPASS;
        copy(info->name,sizeof(info->name),param.name.c_str());info->min_value=param.minimum;info->max_value=param.maximum;info->default_value=param.defaultValue;return true;
    },
    [](const clap_plugin_t* p,clap_id id,double* value)->bool{
        const auto& instance = Instance::get(p);
        const auto& bank = instance.engine.parameters();
        if (!value || !bank.find(id)) return false;
        *value = instance.hostValue(id, bank.get(id));
        return true;
    },
    [](const clap_plugin_t* p,clap_id id,double value,char* dest,std::uint32_t capacity)->bool{
        const auto& instance = Instance::get(p);
        const auto& bank = instance.engine.parameters();
        const auto index = bank.indexOf(id);
        if (index >= bank.descriptors().size() || !dest || !capacity) return false;
        try{auto text=hostField(instance,index).text(value);copy(dest,capacity,text.c_str());return text.size()<capacity;}catch(...){return false;}
    },
    [](const clap_plugin_t* p,clap_id id,const char* text,double* value)->bool{
        const auto& instance = Instance::get(p);
        const auto& bank = instance.engine.parameters();
        const auto index = bank.indexOf(id);
        if (index >= bank.descriptors().size() || !text || !value) return false;
        try {
            const auto parsed = hostField(instance, index).parseText(text);
            if (!parsed) return false;
            *value = *parsed;
            return true;
        } catch (...) { return false; }
    },
    [](const clap_plugin_t* p,const clap_input_events_t* in,const clap_output_events_t* out){
        auto& i = Instance::get(p);
        i.outputEdits(out);
        bool applied{};
        const auto count = in && in->size && in->get ? in->size(in) : 0;
        for (std::uint32_t n = 0; n < count; ++n) {
            if (const auto edit = hostParameterEdit(i, in->get(in, n))) {
                i.engine.applyParameterEvents(std::span(&*edit, 1));
                applied = true;
            }
        }
        publishTail(i, i.active);
        if (applied) {
            if (i.active) requestPresentationCheck(i);
            else onMainThread(p);
        }
    }
};
const clap_plugin_state_t state{
    [](const clap_plugin_t* p,const clap_ostream_t* stream)->bool{
        if(!stream||!stream->write)return false;
        try{const auto bytes=Instance::get(p).engine.saveState();std::size_t offset{};
            while(offset<bytes.size()){const auto written=stream->write(stream,bytes.data()+offset,bytes.size()-offset);if(written<=0||std::uint64_t(written)>bytes.size()-offset)return false;offset+=std::size_t(written);}return true;
        }catch(...){return false;}
    },
    [](const clap_plugin_t* p,const clap_istream_t* stream)->bool{
        if(!stream||!stream->read)return false;
        if(Instance::get(p).processing&&Instance::get(p).engine.stateRequiresStoppedProcessing())return false;
        try{std::vector<std::byte> bytes;std::array<std::byte,4096> chunk;
            for(;;){const auto read=stream->read(stream,chunk.data(),chunk.size());if(read<0||read>std::int64_t(chunk.size()))return false;if(!read)break;
                if(bytes.size()+std::size_t(read)>128*1024*1024)return false;
                bytes.insert(bytes.end(),chunk.begin(),chunk.begin()+read);}
            auto& i=Instance::get(p);if(!i.engine.loadState(bytes))return false;
            if(!i.active)publishTail(i,false);
            try { refreshPresentation(i, true); } catch (...) {}
            return true;
        }catch(...){return false;}
    }
};
#if defined(CELL_CLAP_EXPOSE_PROGRAMS)
std::optional<ProgramAddress> presetAddress(std::string_view key) noexcept {
    ProgramAddress address{};
    const auto colon = key.find(':');
    if (colon == std::string_view::npos) return {};
    const auto end = key.data() + key.size();
    const auto bank = std::from_chars(key.data(), key.data() + colon, address.bank);
    const auto program = std::from_chars(key.data() + colon + 1, end, address.program);
    if (bank.ec != std::errc{} || bank.ptr != key.data() + colon || program.ec != std::errc{} ||
        program.ptr != end || address.bank > 16383 || address.program > 127) return {};
    return address;
}

const clap_plugin_preset_load_t presetLoad{
    [](const clap_plugin_t* plugin, std::uint32_t kind, const char* location, const char* key) {
        auto& instance = Instance::get(plugin);
        auto* memory = dynamic_cast<ProgramMemory*>(&instance.engine.processor());
        if (!memory || kind != CLAP_PRESET_DISCOVERY_LOCATION_PLUGIN || location || !key ||
            instance.engine.activeGestures()) return false;
        try {
            const auto address = presetAddress(key);
            const auto catalog = memory->programs(instance.engine.parameters());
            if (!address || !web::detail::findProgram(catalog, *address) ||
                !memory->recallProgram(instance.engine, *address)) return false;
        } catch (...) { return false; }
        try { refreshPresentation(instance, true); } catch (...) {}
        if (instance.hostState && instance.hostState->mark_dirty) instance.hostState->mark_dirty(instance.host);
        auto* hostLoad = static_cast<const clap_host_preset_load_t*>(
            instance.host->get_extension(instance.host, CLAP_EXT_PRESET_LOAD));
        if (!hostLoad) hostLoad = static_cast<const clap_host_preset_load_t*>(
            instance.host->get_extension(instance.host, CLAP_EXT_PRESET_LOAD_COMPAT));
        if (hostLoad && hostLoad->loaded) hostLoad->loaded(instance.host, kind, location, key);
        return true;
    }
};

const clap_preset_discovery_provider_descriptor_t& presetProviderDescriptor() {
    static const clap_preset_discovery_provider_descriptor_t descriptor{
        CLAP_VERSION, clapDescriptor().id, clapDescriptor().name, clapDescriptor().vendor};
    return descriptor;
}

struct PresetProvider {
    clap_preset_discovery_provider_t provider{};
    const clap_preset_discovery_indexer_t* indexer{};
    ProgramCatalog catalog;
    static PresetProvider& get(const clap_preset_discovery_provider_t* provider) {
        return *static_cast<PresetProvider*>(provider->provider_data);
    }
};

const clap_preset_discovery_factory_t presetFactory{
    [](const clap_preset_discovery_factory_t*) -> std::uint32_t { return 1; },
    [](const clap_preset_discovery_factory_t*, std::uint32_t index) {
        return index ? nullptr : &presetProviderDescriptor();
    },
    [](const clap_preset_discovery_factory_t*, const clap_preset_discovery_indexer_t* indexer, const char* id)
        -> const clap_preset_discovery_provider_t* {
        if (!indexer || !indexer->declare_location || !id || std::strcmp(id, presetProviderDescriptor().id) ||
            !clap_version_is_compatible(indexer->clap_version)) return nullptr;
        try {
            auto provider = std::make_unique<PresetProvider>();
            provider->indexer = indexer;
            provider->provider = {&presetProviderDescriptor(), provider.get(),
                [](const clap_preset_discovery_provider_t* base) {
                    auto& self = PresetProvider::get(base);
                    try {
                        Engine engine(cell::plugin::descriptor(), cell::plugin::createProcessor());
                        const auto* memory = dynamic_cast<const ProgramMemory*>(&engine.processor());
                        if (!memory) return false;
                        self.catalog = memory->programs(engine.parameters());
                        web::detail::validateProgramCatalog(self.catalog);
                        const clap_preset_discovery_location_t location{CLAP_PRESET_DISCOVERY_IS_FACTORY_CONTENT,
                            clapDescriptor().name, CLAP_PRESET_DISCOVERY_LOCATION_PLUGIN, nullptr};
                        return self.indexer->declare_location(self.indexer, &location);
                    } catch (...) { return false; }
                },
                [](const clap_preset_discovery_provider_t* base) { delete &PresetProvider::get(base); },
                [](const clap_preset_discovery_provider_t* base, std::uint32_t kind, const char* location,
                   const clap_preset_discovery_metadata_receiver_t* receiver) {
                    if (kind != CLAP_PRESET_DISCOVERY_LOCATION_PLUGIN || location || !receiver ||
                        !receiver->begin_preset || !receiver->add_plugin_id) return false;
                    try {
                        const clap_universal_plugin_id_t pluginId{"clap", clapDescriptor().id};
                        for (const auto& bank : PresetProvider::get(base).catalog.banks)
                            for (const auto& slot : bank.programs) {
                                const auto key = std::to_string(bank.number) + ":" + std::to_string(slot.number);
                                if (!receiver->begin_preset(receiver, slot.name.c_str(), key.c_str())) return false;
                                receiver->add_plugin_id(receiver, &pluginId);
                                if (receiver->add_extra_info) receiver->add_extra_info(receiver, "bank", bank.name.c_str());
                            }
                        return true;
                    } catch (...) { return false; }
                },
                [](const clap_preset_discovery_provider_t*, const char*) -> const void* { return nullptr; }};
            return &provider.release()->provider;
        } catch (...) { return nullptr; }
    }
};
#endif
const clap_plugin_latency_t latency{[](const clap_plugin_t* p)->std::uint32_t{return Instance::get(p).reportedLatency;}};
const clap_plugin_tail_t tail{[](const clap_plugin_t* p)->std::uint32_t{return Instance::get(p).reportedTail.load(std::memory_order_relaxed);}};

#if CELL_HAS_UI
#if defined(_WIN32)
constexpr auto nativeApi=CLAP_WINDOW_API_WIN32;
#elif defined(__APPLE__)
constexpr auto nativeApi=CLAP_WINDOW_API_COCOA;
#else
constexpr auto nativeApi=CLAP_WINDOW_API_X11;
#endif
const clap_plugin_timer_support_t timerSupport{
    [](const clap_plugin_t* p,clap_id id){auto& i=Instance::get(p);if(i.timer==id&&i.window)i.window->poll();}
};
const clap_plugin_gui_t gui{
    [](const clap_plugin_t* p,const char* api,bool floating)->bool{
        if(!api||floating||std::strcmp(api,nativeApi))return false;
#if !defined(_WIN32) && !defined(__APPLE__)
        if(!Instance::get(p).timers)return false;
#else
        (void)p;
#endif
        return true;
    },
    [](const clap_plugin_t* p,const char** api,bool* floating)->bool{
        if(!api||!floating)return false;
#if !defined(_WIN32) && !defined(__APPLE__)
        if(!Instance::get(p).timers)return false;
#else
        (void)p;
#endif
        *api=nativeApi;*floating=false;return true;
    },
    [](const clap_plugin_t* p,const char* api,bool floating)->bool{
        auto& i=Instance::get(p);if(i.editor||floating||!api||std::strcmp(api,nativeApi))return false;
        try{i.editor=cell::plugin::createEditor(i.engine);i.editor->editsAvailable=[&i]{if(i.hostParams)i.hostParams->request_flush(i.host);if(i.hostState)i.hostState->mark_dirty(i.host);};
            i.editor->productCommand=[&i](std::span<const std::byte> bytes){auto* target=dynamic_cast<cell::CommandTarget*>(&i.engine.processor());if(!target||!target->executeCommand(bytes))return false;if(i.hostState)i.hostState->mark_dirty(i.host);return true;};
            i.editor->transientInput=[&i](std::span<const std::byte> bytes){if(bytes.empty()||bytes.size()>cell::maxProductQueryBytes||!i.engine.executeProductCommand(bytes))return false;if(i.host&&i.host->request_process)i.host->request_process(i.host);return true;};
            i.editor->productQuery=[&i](std::span<const std::byte> bytes,bool){return i.engine.queryProduct(bytes);};
            i.editor->restoreProductState=[&i](std::span<const std::byte> bytes){if(i.processing&&i.engine.stateRequiresStoppedProcessing())return false;if(!i.engine.loadState(bytes))return false;try{refreshPresentation(i,true);}catch(...){}if(i.hostState)i.hostState->mark_dirty(i.host);return true;};
            i.editor->hostParameterMenuAvailable=[&i](ParamId id){const auto* field=i.engine.parameters().find(id);return field&&field->automatable&&i.window&&i.window->isOpen()&&i.hostContextMenu&&i.hostContextMenu->can_popup&&i.hostContextMenu->popup&&i.hostContextMenu->can_popup(i.host);};
            i.editor->showHostParameterMenu=[&i](ParamId id,ui::Point at){
                if(!i.editor->hostParameterMenuAvailable(id))return false;const auto point=i.window->toNativePoint(at);
                if(!std::isfinite(point.x)||!std::isfinite(point.y)||point.x<0||point.y<0||point.x>INT32_MAX||point.y>INT32_MAX)return false;
                const clap_context_menu_target_t target{CLAP_CONTEXT_MENU_TARGET_KIND_PARAM,id};return i.hostContextMenu->popup(i.host,&target,-1,int32_t(std::lround(point.x)),int32_t(std::lround(point.y)));
            };
            const auto preferred=i.editor->preferredSize();i.guiWidth=std::uint32_t(preferred.x*i.scale);i.guiHeight=std::uint32_t(preferred.y*i.scale);
            i.editor->requestSize=[&i](int w,int h){
                if(!i.hostGui||!i.hostGui->request_resize)return false;
                if(i.hostGui->resize_hints_changed)i.hostGui->resize_hints_changed(i.host);
                const auto width=std::uint32_t(w*i.scale),height=std::uint32_t(h*i.scale);
                if(!i.hostGui->request_resize(i.host,width,height))return false;
                if(i.window&&i.window->isOpen()&&!i.window->resize(int(width),int(height)))return false;
                i.guiWidth=width;i.guiHeight=height;return true;
            };
            i.window=std::make_unique<ui::Window>(*i.editor);return true;}catch(...){i.window.reset();i.editor.reset();return false;}
    },
    [](const clap_plugin_t* p){auto& i=Instance::get(p);if(i.timer!=CLAP_INVALID_ID&&i.timers)i.timers->unregister_timer(i.host,i.timer);i.timer=CLAP_INVALID_ID;i.window.reset();i.editor.reset();},
    [](const clap_plugin_t* p,double scale)->bool{auto& i=Instance::get(p);if(!std::isfinite(scale)||scale<.5||scale>4)return false;const auto w=std::uint32_t(i.guiWidth*scale/i.scale),h=std::uint32_t(i.guiHeight*scale/i.scale);if(i.window&&i.window->isOpen()&&!i.window->resize(int(w),int(h)))return false;i.scale=scale;i.guiWidth=w;i.guiHeight=h;return true;},
    [](const clap_plugin_t* p,std::uint32_t* w,std::uint32_t* h)->bool{if(!w||!h)return false;auto& i=Instance::get(p);if(!i.editor)return false;*w=i.guiWidth;*h=i.guiHeight;return true;},
    [](const clap_plugin_t* p)->bool{return bool(Instance::get(p).editor);},
    [](const clap_plugin_t* p,clap_gui_resize_hints_t* hints)->bool{auto& i=Instance::get(p);if(!hints||!i.editor)return false;const auto size=i.editor->preferredSize();*hints={true,true,i.editor->preserveAspectRatio(),std::uint32_t(size.x),std::uint32_t(size.y)};return true;},
    [](const clap_plugin_t* p,std::uint32_t* w,std::uint32_t* h)->bool{auto& i=Instance::get(p);if(!w||!h||!i.editor)return false;const auto size=i.editor->constrainSize({*w/i.scale,*h/i.scale});*w=std::uint32_t(size.x*i.scale);*h=std::uint32_t(size.y*i.scale);return true;},
    [](const clap_plugin_t* p,std::uint32_t w,std::uint32_t h)->bool{auto& i=Instance::get(p);if(!i.editor||!w||!h||w>9600||h>7200)return false;if(i.window&&i.window->isOpen()&&!i.window->resize(int(w),int(h)))return false;i.guiWidth=w;i.guiHeight=h;return true;},
    [](const clap_plugin_t* p,const clap_window_t* parent)->bool{
        auto& i=Instance::get(p);if(!i.window||!parent||!parent->api||std::strcmp(parent->api,nativeApi))return false;
        std::uintptr_t handle{};
#if defined(_WIN32)
        handle=reinterpret_cast<std::uintptr_t>(parent->win32);
#elif defined(__APPLE__)
        handle=reinterpret_cast<std::uintptr_t>(parent->cocoa);
#else
        handle=parent->x11;
#endif
        try{return handle&&i.window->open(handle,int(i.guiWidth),int(i.guiHeight));}catch(...){return false;}
    },
    [](const clap_plugin_t*,const clap_window_t*)->bool{return false;},
    [](const clap_plugin_t*,const char*){},
    [](const clap_plugin_t* p)->bool{
        auto& i=Instance::get(p);if(!i.window||!i.window->isOpen())return false;
#if !defined(_WIN32) && !defined(__APPLE__)
        if(i.timer==CLAP_INVALID_ID&&(!i.timers||!i.timers->register_timer(i.host,33,&i.timer)))return false;
#endif
        i.window->show(true);i.window->poll();return true;
    },
    [](const clap_plugin_t* p)->bool{
        auto& i=Instance::get(p);if(!i.window)return false;i.window->show(false);
        if(i.timer!=CLAP_INVALID_ID&&i.timers){i.timers->unregister_timer(i.host,i.timer);i.timer=CLAP_INVALID_ID;}
        return true;
    }
};
#endif

#if defined(CELL_CLAP_WEB_EDITOR)
#if defined(_WIN32)
constexpr auto& webWindowApi = CLAP_WINDOW_API_WIN32;

std::uintptr_t webParent(const clap_window_t& parent) {
    return reinterpret_cast<std::uintptr_t>(parent.win32);
}
#elif defined(__APPLE__)
constexpr auto& webWindowApi = CLAP_WINDOW_API_COCOA;

std::uintptr_t webParent(const clap_window_t& parent) {
    return reinterpret_cast<std::uintptr_t>(parent.cocoa);
}
#else
constexpr auto& webWindowApi = CLAP_WINDOW_API_X11;

std::uintptr_t webParent(const clap_window_t& parent) { return parent.x11; }
#endif

void webLog(const Instance& instance, const char* message) {
    if (instance.hostLog && instance.hostLog->log)
        instance.hostLog->log(instance.host, CLAP_LOG_WARNING, message);
}

bool webApiSupported([[maybe_unused]] const Instance& instance, const char* api, bool floating) {
    if (!api || floating || std::strcmp(api, webWindowApi)) return false;
#if defined(__APPLE__)
    if (!pthread_main_np()) return false;
#else
    if (!instance.host->request_callback &&
        (!instance.timers || !instance.timers->register_timer || !instance.timers->unregister_timer)) return false;
#endif
    try { return web::View::availability().available; }
    catch (...) { return false; }
}

void updateWebEditor(Instance& instance) {
    auto& editor = *instance.webEditor;
    bool edits = false, cancelled = false, startupFailed = false;
    try {
        for (const auto& event : editor.view.poll()) {
            if (event.kind == web::Event::Kind::loaded) {
                editor.loaded = true;
                editor.lastParameters.clear();
                editor.lastMeters.clear();
            } else if (event.kind == web::Event::Kind::message && editor.loaded) {
                edits |= editor.parameters.apply(event.text);
                auto reply = editor.parameters.takeReply();
                if (!reply.empty()) editor.view.send(reply);
                editor.lastParameters.clear();
            } else if (event.kind == web::Event::Kind::error) {
                cancelled |= editor.parameters.activeGestures() != 0 || editor.parameters.activeActions() != 0;
                editor.parameters.cancel();
                webLog(instance, event.text.c_str());
                if (!editor.loaded) {
                    startupFailed = true;
                    break;
                }
            }
        }
        if (editor.loaded) {
#if defined(CELL_CLAP_EDITOR_FILE_TYPES)
            editor.pollFile();
            auto reply = editor.parameters.takeReply();
            if (!reply.empty()) editor.view.send(reply);
#endif
            refreshPresentation(instance);
            auto send = [&](std::string snapshot, std::string& previous) {
                if (snapshot != previous && editor.view.send(snapshot)) previous = std::move(snapshot);
            };
            if (editor.parameters.usesParameterDeltas()) {
                editor.parameters.updateParameters([&](const std::string& message) { return editor.view.send(message); });
            } else send(editor.parameters.snapshot(), editor.lastParameters);
            const auto reduction = instance.gainReduction
                ? std::optional{instance.reductionDb.load(std::memory_order_relaxed)} : std::nullopt;
            send(web::OutputMeter::snapshot(instance.editorMeter.levels(), instance.editorInputMeter.levels(), reduction),
                 editor.lastMeters);
        }
    } catch (...) {
        cancelled |= editor.parameters.activeGestures() != 0 || editor.parameters.activeActions() != 0;
        editor.parameters.cancel();
        webLog(instance, "Cell CLAP editor update failed");
        startupFailed = !editor.loaded;
    }
    if (startupFailed) {
        closeWebView(instance, true);
        if (instance.hostGui && instance.hostGui->closed) instance.hostGui->closed(instance.host, false);
        return;
    }
    if (edits && instance.hostState && instance.hostState->mark_dirty)
        instance.hostState->mark_dirty(instance.host);
    if ((edits || cancelled) && instance.hostParams && instance.hostParams->request_flush)
        instance.hostParams->request_flush(instance.host);
}

#if !defined(__APPLE__)
const clap_plugin_timer_support_t webTimerSupport{
    [](const clap_plugin_t* plugin, clap_id id) {
        auto& instance = Instance::get(plugin);
        if (instance.webEditor && webEditorRunning(instance) && instance.timer == id) updateWebEditor(instance);
    }
};
#endif

bool startWebEditorTimer(Instance& instance) {
#if defined(__APPLE__)
    instance.webEditor->updateTimer = web::cocoa::startTimer(&instance,
        [](CFRunLoopTimerRef, void* data) { updateWebEditor(*static_cast<Instance*>(data)); });
    return instance.webEditor->updateTimer != nullptr;
#else
    clap_id timer = CLAP_INVALID_ID;
    if (instance.timers && instance.timers->register_timer && instance.timers->unregister_timer &&
        instance.timers->register_timer(instance.host, 33, &timer) && timer != CLAP_INVALID_ID) {
        instance.timer = timer;
        return true;
    }
    if (!instance.host->request_callback) return false;
    auto& editor = *instance.webEditor;
    editor.fallbackTimer = std::jthread([&instance, &editor](std::stop_token stop) {
        std::mutex mutex;
        std::condition_variable_any timeout;
        std::unique_lock lock(mutex);
        while (!stop.stop_requested()) {
            if (!editor.fallbackTick.exchange(true, std::memory_order_acq_rel))
                instance.host->request_callback(instance.host);
            timeout.wait_for(lock, stop, std::chrono::milliseconds(33), [] { return false; });
        }
    });
    return true;
#endif
}

const clap_plugin_gui_t webGui{
    [](const clap_plugin_t* plugin, const char* api, bool floating) {
        return webApiSupported(Instance::get(plugin), api, floating);
    },
    [](const clap_plugin_t* plugin, const char** api, bool* floating) {
        if (!api || !floating || !webApiSupported(Instance::get(plugin), webWindowApi, false)) return false;
        *api = webWindowApi;
        *floating = false;
        return true;
    },
    [](const clap_plugin_t* plugin, const char* api, bool floating) {
        auto& instance = Instance::get(plugin);
        if (instance.webEditor || !webApiSupported(instance, api, floating)) return false;
        try {
            instance.webEditor = std::make_unique<WebEditor>(instance.engine);
            instance.guiWidth = std::clamp(instance.guiWidth, 1u, 8192u);
            instance.guiHeight = std::clamp(instance.guiHeight, 1u, 8192u);
            return true;
        } catch (const std::exception& e) { webLog(instance, e.what()); return false; }
    },
    [](const clap_plugin_t* plugin) {
        auto& instance = Instance::get(plugin);
        closeWebView(instance, true);
        instance.webEditor.reset();
    },
    [](const clap_plugin_t*, double) { return false; }, // Use the system browser's scale.
    [](const clap_plugin_t* plugin, std::uint32_t* width, std::uint32_t* height) {
        const auto& instance = Instance::get(plugin);
        if (!instance.webEditor || !width || !height) return false;
        *width = instance.guiWidth;
        *height = instance.guiHeight;
        return true;
    },
    [](const clap_plugin_t* plugin) { return bool(Instance::get(plugin).webEditor); },
    [](const clap_plugin_t* plugin, clap_gui_resize_hints_t* hints) {
        if (!Instance::get(plugin).webEditor || !hints) return false;
        *hints = {true, true, false, 0, 0};
        return true;
    },
    [](const clap_plugin_t* plugin, std::uint32_t* width, std::uint32_t* height) {
        if (!Instance::get(plugin).webEditor || !width || !height) return false;
        *width = std::clamp(*width, 1u, 8192u);
        *height = std::clamp(*height, 1u, 8192u);
        return true;
    },
    [](const clap_plugin_t* plugin, std::uint32_t width, std::uint32_t height) {
        auto& instance = Instance::get(plugin);
        if (!instance.webEditor || !width || !height || width > 8192 || height > 8192) return false;
        try {
            if (webEditorRunning(instance))
                instance.webEditor->view.setBounds({0, 0, int(width), int(height)});
            instance.guiWidth = width;
            instance.guiHeight = height;
            return true;
        } catch (...) { return false; }
    },
    [](const clap_plugin_t* plugin, const clap_window_t* parent) {
        auto& instance = Instance::get(plugin);
        if (!instance.webEditor || !parent || !parent->api || std::strcmp(parent->api, webWindowApi)) return false;
        const auto window = webParent(*parent);
        if (!window || (webEditorRunning(instance) && instance.webEditor->parent != window)) return false;
        instance.webEditor->parent = window;
        return true;
    },
    [](const clap_plugin_t*, const clap_window_t*) { return false; },
    [](const clap_plugin_t*, const char*) {},
    [](const clap_plugin_t* plugin) {
        auto& instance = Instance::get(plugin);
        if (!instance.webEditor || !instance.webEditor->parent) return false;
        if (webEditorRunning(instance)) return true;
        auto& editor = *instance.webEditor;
        try {
            web::Options options;
            options.parent = editor.parent;
            options.bounds = {0, 0, int(instance.guiWidth), int(instance.guiHeight)};
#if defined(CELL_CLAP_EDITOR_ASSETS)
            options.assets = assets::clapEditorPageAssets();
#endif
#if defined(_WIN32)
            const auto parent = reinterpret_cast<HWND>(options.parent);
            if (GetWindowThreadProcessId(parent, nullptr) != GetCurrentThreadId()) {
                webLog(instance, "The WebView2 parent must belong to the CLAP GUI thread");
                return false;
            }
            options.userDataDirectory = web::win32::pluginProfile(L"CLAP", "plugin-" CELL_CLAP_WEB_PROFILE_ID);
            if (!web::win32::pluginModule()) {
                webLog(instance, "Cannot retain the CLAP module for pending browser callbacks");
                return false;
            }
#elif defined(__APPLE__)
            options.boundsUnits = web::BoundsUnits::parentCoordinates;
            if (!web::cocoa::retainPluginModule()) {
                webLog(instance, "Cannot retain the CLAP module for pending browser callbacks");
                return false;
            }
#endif
            const std::string_view html(reinterpret_cast<const char*>(assets::clapEditorPage),
                                        sizeof(assets::clapEditorPage));
            if (!editor.view.open(options) || !editor.view.loadHtml(html)) {
                const auto problem = editor.view.error();
                webLog(instance, problem.empty() ? "Cell CLAP editor page could not be loaded" : problem.c_str());
            } else if (startWebEditorTimer(instance)) {
                instance.setEditorMeters(true);
                return true;
            } else {
                webLog(instance, "Cell CLAP editor timer registration failed");
            }
        } catch (const std::exception& e) { webLog(instance, e.what()); }
        closeWebView(instance, false);
        return false;
    },
    [](const clap_plugin_t* plugin) {
        auto& instance = Instance::get(plugin);
        if (!instance.webEditor) return false;
        closeWebView(instance, true);
        return true;
    }
};
#endif

const clap_plugin_render_t renderMode{
    [](const clap_plugin_t*)->bool{return false;},
    [](const clap_plugin_t* p,clap_plugin_render_mode mode)->bool{if(mode!=CLAP_RENDER_REALTIME&&mode!=CLAP_RENDER_OFFLINE)return false;Instance::get(p).offline=mode==CLAP_RENDER_OFFLINE;return true;}
};
const void* CLAP_ABI extension(const clap_plugin_t*,const char* id) {
    if(!id)return nullptr;
    if(!std::strcmp(id,CLAP_EXT_RENDER))return &renderMode;
    if(!std::strcmp(id,CLAP_EXT_AUDIO_PORTS))return &ports;
    if(portWidths().size()>1) {
        if(!std::strcmp(id,CLAP_EXT_AUDIO_PORTS_CONFIG))return &portConfigurations;
        if(!std::strcmp(id,CLAP_EXT_CONFIGURABLE_AUDIO_PORTS)||!std::strcmp(id,"clap.configurable-audio-ports.draft1"))
            return &configurablePorts;
        if(!std::strcmp(id,CLAP_EXT_AUDIO_PORTS_CONFIG_INFO)||!std::strcmp(id,"clap.audio-ports-config-info/draft-0"))
            return &portConfigurationInfo;
    }
    if((!std::strcmp(id,CLAP_EXT_SURROUND)||!std::strcmp(id,"clap.surround.draft/4"))&&supportsSurround())return &surround;
    if(!std::strcmp(id,CLAP_EXT_PARAMS))return &params;
    if(!std::strcmp(id,CLAP_EXT_NOTE_PORTS)&&(hasNotePort(true)||hasNotePort(false)))return &notePorts;
    if(!std::strcmp(id,CLAP_EXT_STATE))return &state;
#if defined(CELL_CLAP_EXPOSE_PROGRAMS)
    if (!std::strcmp(id, CLAP_EXT_PRESET_LOAD) || !std::strcmp(id, CLAP_EXT_PRESET_LOAD_COMPAT)) return &presetLoad;
#endif
    if(!std::strcmp(id,CLAP_EXT_LATENCY))return &latency;
    if(!std::strcmp(id,CLAP_EXT_TAIL))return &tail;
#if CELL_HAS_UI
    if(!std::strcmp(id,CLAP_EXT_GUI))return &gui;
    if(!std::strcmp(id,CLAP_EXT_TIMER_SUPPORT))return &timerSupport;
#elif defined(CELL_CLAP_WEB_EDITOR)
    if(!std::strcmp(id,CLAP_EXT_GUI))return &webGui;
#if !defined(__APPLE__)
    if(!std::strcmp(id,CLAP_EXT_TIMER_SUPPORT))return &webTimerSupport;
#endif
#endif
    return nullptr;
}
const clap_plugin_factory_t factory{
    [](const clap_plugin_factory_t*)->std::uint32_t{return 1;},
    [](const clap_plugin_factory_t*,std::uint32_t index)->const clap_plugin_descriptor_t*{return index?nullptr:&clapDescriptor();},
    [](const clap_plugin_factory_t*,const clap_host_t* host,const char* id)->const clap_plugin_t*{
        if(portWidths().empty()||!host||!id||!host->get_extension||!clap_version_is_compatible(host->clap_version)||std::strcmp(id,clapDescriptor().id))return nullptr;
        try{auto* i=new Instance;i->host=host;i->plugin={&clapDescriptor(),i,init,destroy,activate,deactivate,start,stop,reset,process,extension,onMainThread};return &i->plugin;}catch(...){return nullptr;}
    }
};
}
namespace {
std::mutex moduleEntriesMutex;
std::uint64_t moduleEntries{};
bool enterModule() {
    std::lock_guard lock(moduleEntriesMutex);
    if (moduleEntries == UINT64_MAX) return false;
    try { if (!moduleEntries) cell::jobs::resumeModulePools(); }
    catch (...) { return false; }
    ++moduleEntries;
    return true;
}
void leaveModule() {
    std::lock_guard lock(moduleEntriesMutex);
    if (!moduleEntries || --moduleEntries) return;
    try { cell::jobs::quiesceModulePools();
#if CELL_HAS_UI && defined(_WIN32)
        cell::ui::shutdownNativePlatformServices();
#endif
    } catch (...) {}
}
}
extern "C" {
CLAP_EXPORT extern const clap_plugin_entry_t clap_entry{
    CLAP_VERSION,[](const char*)->bool{return enterModule();},[](){leaveModule();},
    [](const char* id)->const void* {
        if (!id) return nullptr;
        if (!std::strcmp(id, CLAP_PLUGIN_FACTORY_ID)) return &factory;
#if defined(CELL_CLAP_EXPOSE_PROGRAMS)
        if (!std::strcmp(id, CLAP_PRESET_DISCOVERY_FACTORY_ID) || !std::strcmp(id, CLAP_PRESET_DISCOVERY_FACTORY_ID_COMPAT))
            return &presetFactory;
#endif
        return nullptr;
    }
};
}
