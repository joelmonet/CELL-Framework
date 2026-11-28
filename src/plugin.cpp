#include <cell/plugin.hpp>
#include <cell/realtime.hpp>
#include <cell/state_transfer.hpp>
#include <cell/text.hpp>
#include <set>

namespace cell {
namespace {
std::string_view trimParameterText(std::string_view text) noexcept {
    constexpr std::string_view whitespace = " \t\r\n\f\v";
    const auto first = text.find_first_not_of(whitespace);
    if (first == std::string_view::npos) return {};
    return text.substr(first, text.find_last_not_of(whitespace) - first + 1);
}

bool equalParameterText(std::string_view left, std::string_view right) {
    return left == right || Text(left).equalsIgnoreCase(Text(right));
}
}

std::optional<double> Parameter::parseText(std::string_view input) const {
    input = trimParameterText(input);
    if (input.empty()) return {};
    auto label = input;
    if (!unit.empty() && input.size() > unit.size() && equalParameterText(input.substr(input.size() - unit.size()), unit)) {
        const auto prefix = input.substr(0, input.size() - unit.size());
        if (trimParameterText(prefix).size() != prefix.size()) label = trimParameterText(prefix);
    }
    std::optional<double> match;
    for (std::size_t i = 0; i < choices.size(); ++i) {
        const auto choice = trimParameterText(choices[i]);
        if (!equalParameterText(choice, input) && !equalParameterText(choice, label)) continue;
        if (match) return {};
        match = minimum + static_cast<double>(i);
    }
    if (match) return match;
    if (input.front() == '+') input.remove_prefix(1);
    if (input.empty()) return {};
    double parsed{};
    const auto end = input.data() + input.size();
    const auto result = detail::parseDoubleChars(input.data(), end, parsed);
    const auto suffix = trimParameterText({result.ptr, static_cast<std::size_t>(end - result.ptr)});
    if (result.ec != std::errc{} || !std::isfinite(parsed) || (!suffix.empty() && !equalParameterText(suffix, unit)))
        return {};
    return clamp(parsed);
}

ParameterPresentation resolveParameterPresentation(const Parameter& storage, const ParameterPresentationSource* source) {
    if (source) {
        if (auto metadata = source->parameterPresentation(storage.id)) {
            auto& field = metadata->field;
            if (!field.name.empty() && std::isfinite(field.minimum) && std::isfinite(field.maximum) &&
                field.minimum < field.maximum && std::isfinite(field.defaultValue) &&
                field.defaultValue >= field.minimum && field.defaultValue <= field.maximum &&
                std::isfinite(field.step) && field.step >= 0 && std::isfinite(field.skew) && field.skew > 0 &&
                (field.choices.empty() || (field.step == 1 && std::floor(field.minimum) == field.minimum &&
                   field.maximum - field.minimum + 1 == static_cast<double>(field.choices.size())))) {
                field.id = storage.id;
                field.key = storage.key;
                field.bypass = storage.bypass;
                return std::move(*metadata);
            }
        }
    }
    return {storage};
}

bool sameParameterDomain(const Parameter& left, const Parameter& right) noexcept {
    return left.minimum == right.minimum && left.maximum == right.maximum &&
        left.step == right.step && left.skew == right.skew;
}

double mapParameterValue(double value, const Parameter& from, const Parameter& to) noexcept {
    return sameParameterDomain(from, to) ? value : to.fromNormalized(from.toNormalized(value));
}

class Engine::QueuedMidi {
public:
    bool push(MidiEvent event) noexcept {
        const auto write = write_.load(std::memory_order_relaxed);
        const auto next = (write + 1) % slots_.size();
        if (next == read_.load(std::memory_order_acquire)) return false;
        auto& slot = slots_[write];
        event.frame = 0;
        if (event.isSysEx()) {
            if (event.sysEx.size() > slot.bytes.size() || !midi::validSysEx(event.sysEx)) return false;
            std::ranges::copy(event.sysEx, slot.bytes.begin());
            event.sysEx = std::span(slot.bytes).first(event.sysEx.size());
        } else if (event.isUmp() ? !midi::validUmp(event) : !midi::valid(event)) return false;
        slot.event = event;
        write_.store(next, std::memory_order_release);
        return true;
    }

    void drain(midi::Output& output) noexcept {
        auto read = read_.load(std::memory_order_relaxed);
        const auto end = write_.load(std::memory_order_acquire);
        while (read != end && output.push(slots_[read].event)) {
            read = (read + 1) % slots_.size();
            read_.store(read, std::memory_order_release);
        }
    }

private:
    struct Slot {
        MidiEvent event;
        std::array<std::uint8_t, 65535> bytes;
    };
    std::array<Slot, 5> slots_;
    alignas(64) std::atomic<std::size_t> write_{};
    alignas(64) std::atomic<std::size_t> read_{};
};

Engine::Engine(const PluginDescriptor& descriptor, std::unique_ptr<Processor> processor)
    : id_(descriptor.id?descriptor.id:""),name_(descriptor.name?descriptor.name:""),
      vendor_(descriptor.vendor?descriptor.vendor:""),version_(descriptor.version?descriptor.version:""),
      descriptor_(descriptor), parameters_(descriptor.parameters), processor_(std::move(processor)) {
    const bool invalidMidiOnly = descriptor.channels == 0 && (!descriptor.producesMidi ||
        descriptor.supportsMono || descriptor.supportsSidechain || !descriptor.mainBusLayouts.empty());
    if (!processor_ || descriptor.channels > 64 || invalidMidiOnly || !descriptor.id || !*descriptor.id)
        throw std::invalid_argument("invalid plugin descriptor or processor");
    descriptor_.id=id_.c_str();descriptor_.name=name_.c_str();
    descriptor_.vendor=vendor_.c_str();descriptor_.version=version_.c_str();
    descriptor_.parameters=parameters_.descriptors();
    processor_->attachParameters(parameters_);
    processor_->attachParameterOutput(this);
    processor_->initializeParameters(parameters_);
    if(descriptor_.acceptsMidi){controllerInput_=std::make_unique<midi::ControlInput>();padInput_=std::make_unique<midi::PadInput>();}
    if (descriptor_.producesMidi) queuedMidi_ = std::make_unique<QueuedMidi>();
}

Engine::~Engine() { snapshots_.reset(); }

bool Engine::executeProductCommand(std::span<const std::byte> bytes) {
    auto* target = dynamic_cast<CommandTarget*>(processor_.get());
    return target && target->executeCommand(bytes);
}
bool Engine::controllerMidi(const midi::ControlMessage& message)noexcept {
    return controllerInput_&&controllerInput_->submit(message);
}
void Engine::releaseControllerMidi()noexcept{if(controllerInput_)controllerInput_->releaseAll();}
bool Engine::enqueueMidi(const MidiEvent& event) noexcept {
    return queuedMidi_ && (!event.isSysEx() || descriptor_.supportsSysEx) &&
        (!event.isUmp() || descriptor_.supportsUmp) && queuedMidi_->push(event);
}
std::vector<std::byte> Engine::queryProduct(std::span<const std::byte> bytes) {
    if (state::isTransfer(bytes)) {
        std::lock_guard lock(snapshotsMutex_);
        if (!snapshots_) snapshots_ = std::make_unique<state::SnapshotServer>([this]() -> std::optional<std::vector<std::byte>> {
            if (stateSaveRequiresStoppedProcessing()) return {};
            const auto* codec = dynamic_cast<const StateExtension*>(processor_.get());
            const auto generation = stateGeneration();
            const auto revision = codec ? codec->stateSnapshotRevision() : 0;
            if (codec && !codec->stateSnapshotReady()) return {};
            auto result = saveState();
            if (codec && !codec->stateSnapshotReady()) return {};
            if (generation != stateGeneration() || (codec && revision != codec->stateSnapshotRevision())) return {};
            return result;
        });
        return snapshots_->query(bytes);
    }
    auto* target = dynamic_cast<CommandTarget*>(processor_.get());
    return target ? target->queryProduct(bytes) : std::vector<std::byte>{};
}

bool Engine::parameterOutput(ParamId id,double value) noexcept {
    const auto* field=parameters_.find(id);
    if(!field||!std::isfinite(value)||!generated_.freeCapacity())return false;
    value=field->clamp(value);
    parameters_.set(id,value);
    return generated_.push({EditType::value,id,value});
}

void Engine::prepare(double sampleRate, std::uint32_t maxFrames, std::uint32_t channels) {
    if (!std::isfinite(sampleRate) || sampleRate < 1000 || sampleRate > 768000 ||
        maxFrames == 0 || maxFrames > (1u << 20)) throw std::invalid_argument("invalid process specification");
    if(!channels)channels=descriptor_.channels;
    const bool supported=descriptor_.mainBusLayouts.empty()?
        channels==descriptor_.channels||(descriptor_.supportsMono&&channels==1):
        std::any_of(descriptor_.mainBusLayouts.begin(),descriptor_.mainBusLayouts.end(),[channels](const auto& layout){return channelCount(layout.output)==channels;});
    if(!supported)throw std::invalid_argument("unsupported channel configuration");
    stateGeneration_.fetch_add(1,std::memory_order_release);
    prepared_ = false;
    spec_ = {sampleRate, maxFrames, channels};
    if(controllerInput_)controllerInput_->releaseAll();
    const auto midiCapacity=descriptor_.acceptsMidi?midi::eventCapacity+midi::ControlInput::eventCapacity:0;
    midiScratch_.resize(midiCapacity);controllerScratch_.resize(midiCapacity);hardwareScratch_.resize(descriptor_.acceptsMidi?midi::eventCapacity:0);
    if(padInput_)padInput_->resetAudio();
    processor_->prepare(spec_);
    processor_->reset();
    prepared_ = true;
}

void Engine::reset() noexcept {if(controllerInput_)controllerInput_->releaseAll();if(padInput_)padInput_->resetAudio();processor_->reset();}

void Engine::applyParameterEvent(const ParameterEdit& edit) noexcept {
    if (edit.type == EditType::value) {
        if (parameters_.set(edit.id, edit.value))
            processor_->parameterChanged(edit.id, parameters_.get(edit.id));
    } else if ((edit.type == EditType::begin || edit.type == EditType::end) && parameters_.find(edit.id)) {
        processor_->parameterGesture(edit.id, edit.type);
    }
}

void Engine::applyParameterEvents(std::span<const ParameterEdit> events) noexcept {
    for (const auto& event : events) applyParameterEvent(event);
}

void Engine::process(AudioBlock<float> block, std::span<const ParameterEvent> events,
                     const Transport& transport, std::span<const MidiEvent> midi,AudioBlock<float> sidechain,midi::Output* output) noexcept {
    ScopedNoDenormals denormals;
    if (!prepared_ || block.frames > spec_.maxFrames || block.channelCount != spec_.channels) {
        block.clear();
        return;
    }
    if(padInput_&&padInput_->active()){
        std::size_t count{};
        if(!padInput_->process(midi,hardwareScratch_,block.frames,count)){padInput_->resetAudio();processor_->reset();return;}
        midi={hardwareScratch_.data(),count};
    }
    if(controllerInput_){
        const auto controller=controllerInput_->drain();
        if(!controller.empty()){
            if(midi.size()>midi::eventCapacity){processor_->reset();return;}
            std::copy(controller.begin(),controller.end(),controllerScratch_.begin());
            std::copy(midi.begin(),midi.end(),controllerScratch_.begin()+std::ptrdiff_t(controller.size()));
            midi={controllerScratch_.data(),controller.size()+midi.size()};
        }
    }
    const bool routed=processor_->routesMidi();
    if(routed&&midi.size()>midi::eventCapacity){
        processor_->reset();if(output){output->reject();for(unsigned ch=0;ch<16;++ch)for(unsigned cc:{64u,120u,123u})output->push({0,{std::uint8_t(0xb0|ch),std::uint8_t(cc),0},3});}return;
    }
    if (output && queuedMidi_ && block.frames) queuedMidi_->drain(*output);
    if(output&&!routed)midi::forward(midi,*output,block.frames);
    auto render = [&](std::uint32_t begin, std::uint32_t end) noexcept {
        if (begin == end) return;
        std::span<const MidiEvent> segmentMidi;
        // The common unsplit quantum can borrow its original timestamps. This
        // avoids copying and preserves dense MIDI beyond the rebasing scratch.
        if(begin==0&&std::all_of(midi.begin(),midi.end(),[end](const auto& event){return event.frame<end;}))segmentMidi=midi;
        else {
            std::size_t count{};
            for (const auto& event : midi) {
                if (event.frame >= begin && event.frame < end && count < midiScratch_.size()) {
                    midiScratch_[count] = event;
                    midiScratch_[count++].frame -= begin;
                }
            }
            segmentMidi={midiScratch_.data(),count};
        }
        auto segmentTransport = transport;
        segmentTransport.samplePosition += begin;
        if(transport.playing)segmentTransport.beatPosition += double(begin) / spec_.sampleRate * transport.tempo / 60.0;
        auto side=sidechain.channelCount&&sidechain.frames>=block.frames?sidechain.slice(begin,end-begin):AudioBlock<float>{};
        if(output&&routed){
            struct OffsetOutput final:midi::Output{
                midi::Output& destination;std::uint32_t offset;
                OffsetOutput(midi::Output& d,std::uint32_t o):destination(d),offset(o){}
                bool push(MidiEvent event)noexcept override{event.frame+=offset;return destination.push(event);}
                void reject()noexcept override{destination.reject();}
            } shifted(*output,begin);
            processor_->processMidi(block.slice(begin,end-begin),parameters_,segmentTransport,segmentMidi,side,shifted);
        }else processor_->processSidechain(block.slice(begin, end - begin), parameters_, segmentTransport,
                            segmentMidi,side);
    };
    std::uint32_t cursor{};
    for (const auto& event : events) {
        if (event.frame < cursor || event.frame > block.frames ||
            (event.type == EditType::value && !std::isfinite(event.value)) ||
            !parameters_.find(event.id)) continue;
        render(cursor, event.frame);
        applyParameterEvent({event.type, event.id, event.value});
        cursor = event.frame;
    }
    render(cursor, block.frames);
}

bool Engine::publishUiEdits(std::span<const ParameterEdit> edits) noexcept {
    if (!edits_.pushBatch(edits)) return false;
    if (uiEditObserver_) uiEditObserver_(uiEditContext_, edits);
    return true;
}

bool Engine::gestureActive(ParamId id) const noexcept {
    return std::find(gestureIds_.begin(),gestureIds_.begin()+gestureCount_,id)!=gestureIds_.begin()+gestureCount_;
}

bool Engine::beginEdits(std::span<const ParamId> ids) noexcept {
    if(ids.empty()||ids.size()>maxConcurrentGestures-gestureCount_||edits_.freeCapacity()<gestureReservations_+ids.size()*2)return false;
    std::array<ParameterEdit,maxConcurrentGestures> events{};
    for(std::size_t i=0;i<ids.size();++i){
        if(!parameters_.find(ids[i])||gestureActive(ids[i]))return false;
        for(std::size_t j=0;j<i;++j)if(ids[j]==ids[i])return false;
        events[i]={EditType::begin,ids[i],0};
    }
    if(!publishUiEdits({events.data(),ids.size()}))return false;
    for(std::size_t i=0;i<ids.size();++i){gestureIds_[gestureCount_]=events[i].id;gestureMomentary_[gestureCount_]=false;++gestureCount_;}
    gestureReservations_+=ids.size();
    return true;
}

bool Engine::beginMomentary(ParamId id,double pressed,double released) noexcept {
    const auto* field=parameters_.find(id);
    if(!field||!std::isfinite(pressed)||!std::isfinite(released)||gestureActive(id)||gestureCount_==maxConcurrentGestures||edits_.freeCapacity()<gestureReservations_+4)return false;
    pressed=field->clamp(pressed);released=field->clamp(released);
    const std::array<ParameterEdit,2> events{{{EditType::begin,id,0},{EditType::value,id,pressed}}};
    parameters_.restorePlain(id,pressed);
    if(!publishUiEdits(events))return false;
    gestureIds_[gestureCount_]=id;gestureMomentary_[gestureCount_]=true;gestureReleaseValues_[gestureCount_]=released;
    ++gestureCount_;gestureReservations_+=2;return true;
}

bool Engine::editValues(std::span<const std::pair<ParamId,double>> values) noexcept {
    if(values.empty()||values.size()>maxConcurrentGestures||edits_.freeCapacity()<gestureReservations_+values.size())return false;
    std::array<ParameterEdit,maxConcurrentGestures> events{};
    for(std::size_t i=0;i<values.size();++i){
        const auto [id,value]=values[i];const auto* field=parameters_.find(id);
        if(!field||!std::isfinite(value)||(gestureCount_&&!gestureActive(id)))return false;
        for(std::size_t j=0;j<i;++j)if(values[j].first==id)return false;
        events[i]={EditType::value,id,field->clamp(value)};
    }
    // Only the GUI producer consumes capacity; the consumer can only free it.
    // Publish the complete scalar bank before releasing the ordered batch.
    for(std::size_t i=0;i<values.size();++i)parameters_.restorePlain(events[i].id,events[i].value);
    return publishUiEdits({events.data(),values.size()});
}

bool Engine::endEdits(std::span<const ParamId> ids) noexcept {
    if(ids.empty()||ids.size()>gestureCount_)return false;
    std::array<ParameterEdit,maxConcurrentGestures*2> events{};
    std::array<ParamId,maxConcurrentGestures> closing{};std::size_t count=0;
    for(std::size_t i=0;i<ids.size();++i){
        if(!gestureActive(ids[i]))return false;
        for(std::size_t j=0;j<i;++j)if(ids[j]==ids[i])return false;
        closing[i]=ids[i];const auto index=std::size_t(std::find(gestureIds_.begin(),gestureIds_.begin()+gestureCount_,ids[i])-gestureIds_.begin());
        if(gestureMomentary_[index])events[count++]={EditType::value,ids[i],gestureReleaseValues_[index]};
        events[count++]={EditType::end,ids[i],0};
    }
    if(edits_.freeCapacity()<count)return false;
    for(std::size_t i=0;i<count;++i)if(events[i].type==EditType::value)parameters_.restorePlain(events[i].id,events[i].value);
    if(!publishUiEdits({events.data(),count}))return false;
    for(std::size_t i=0;i<ids.size();++i){
        const auto index=std::size_t(std::find(gestureIds_.begin(),gestureIds_.begin()+gestureCount_,closing[i])-gestureIds_.begin());
        gestureReservations_-=gestureMomentary_[index]?2:1;--gestureCount_;
        gestureIds_[index]=gestureIds_[gestureCount_];gestureMomentary_[index]=gestureMomentary_[gestureCount_];gestureReleaseValues_[index]=gestureReleaseValues_[gestureCount_];
        gestureMomentary_[gestureCount_]=false;
    }
    return true;
}

bool Engine::endAllEdits() noexcept {return !gestureCount_||endEdits({gestureIds_.data(),gestureCount_});}

bool Engine::edit(ParameterEdit edit) noexcept {
    if(!std::isfinite(edit.value))return false;
    if(edit.type==EditType::begin)return beginEdits({&edit.id,1});
    if(edit.type==EditType::end)return endEdits({&edit.id,1});
    if(edit.type==EditType::value){const std::pair<ParamId,double> value{edit.id,edit.value};return editValues({&value,1});}
    return false;
}

bool Engine::editGesture(ParamId id, double value) noexcept {
    const auto* parameter = parameters_.find(id);
    if (gestureCount_ || !parameter || !std::isfinite(value)) return false;
    value = parameter->clamp(value);
    const std::array<ParameterEdit, 3> gesture{{{EditType::begin, id, 0}, {EditType::value, id, value}, {EditType::end, id, 0}}};
    if (edits_.freeCapacity() < gesture.size()) return false;
    parameters_.set(id, value);
    return publishUiEdits(gesture);
}

bool Engine::editTransaction(std::span<const std::pair<ParamId,double>> values) {
    if(gestureCount_||values.size()>682||edits_.freeCapacity()<values.size()*3)return false;
    std::vector<ParameterEdit> events;events.reserve(values.size()*3);
    for(const auto& [id,value]:values){const auto* p=parameters_.find(id);if(!p||!std::isfinite(value))return false;
        events.push_back({EditType::begin,id,0});events.push_back({EditType::value,id,p->clamp(value)});events.push_back({EditType::end,id,0});}
    // Only this producer can consume queue capacity; the consumer only frees it.
    // Commit the bank first, with the caller's revision parameter last.
    for(std::size_t i=1;i<events.size();i+=3)parameters_.restorePlain(events[i].id,events[i].value);
    return publishUiEdits(events);
}

std::vector<std::byte> Engine::saveState() const {
    if(const auto* codec=dynamic_cast<const StateExtension*>(processor_.get()))return codec->saveProductState(parameters_);
    ByteWriter writer;
    writer.u32(0x4c4c4543u); // CELL
    writer.u32(1);
    writer.u32(stableId(descriptor_.id));
    writer.u32(static_cast<std::uint32_t>(descriptor_.parameters.size()));
    for (const auto& p : descriptor_.parameters) { writer.u32(p.id); writer.f64(parameters_.get(p.id)); }
    return std::move(writer.bytes);
}

bool Engine::loadState(std::span<const std::byte> data) {
    if(auto* codec=dynamic_cast<StateExtension*>(processor_.get())){if(!codec->loadProductState(data,parameters_))return false;if(controllerInput_)controllerInput_->releaseAll();stateGeneration_.fetch_add(1,std::memory_order_release);return true;}
    if (data.size() > 1024 * 1024) return false;
    ByteReader reader(data);
    std::uint32_t magic{}, version{}, identity{}, count{};
    if (!reader.u32(magic) || magic != 0x4c4c4543u || !reader.u32(version) || version != 1 ||
        !reader.u32(identity) || identity != stableId(descriptor_.id) || !reader.u32(count) ||
        count > 65536 || reader.remaining() != std::size_t(count) * 12) return false;
    // Validate everything before committing any changes.
    std::vector<std::pair<ParamId, double>> pending;
    std::set<ParamId> seen;
    pending.reserve(count);
    for (std::uint32_t i = 0; i < count; ++i) {
        ParamId id{}; double value{};
        if (!reader.u32(id) || !reader.f64(value) || !std::isfinite(value) || !seen.insert(id).second) return false;
        if (parameters_.find(id)) pending.emplace_back(id, value); // allow removed parameters
    }
    for (auto [id, value] : pending) parameters_.set(id, value);
    if(controllerInput_)controllerInput_->releaseAll();
    stateGeneration_.fetch_add(1,std::memory_order_release);
    return true;
}

} // namespace cell
