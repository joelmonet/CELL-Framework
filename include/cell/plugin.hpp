#pragma once

#include <cell/audio.hpp>
#include <cell/parameters.hpp>
#include <cell/midi.hpp>
#include <cell/midi_control.hpp>
#include <cell/midi_devices.hpp>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>

namespace cell {
namespace state { class SnapshotServer; }

// Named main-bus layouts keep speaker roles distinct from a channel count.
// The 9.1.6 layout uses front-wide speakers in addition to the 7.1 bed.
enum class AudioLayout {mono,stereo,surround51,surround712,surround714,surround916};
constexpr unsigned channelCount(AudioLayout layout)noexcept{
    switch(layout){case AudioLayout::mono:return 1;case AudioLayout::stereo:return 2;case AudioLayout::surround51:return 6;case AudioLayout::surround712:return 10;case AudioLayout::surround714:return 12;case AudioLayout::surround916:return 16;}return 0;
}
struct MainBusLayout {AudioLayout input=AudioLayout::stereo,output=AudioLayout::stereo;};

struct PluginDescriptor {
    const char* id{};
    const char* name{};
    const char* vendor{};
    const char* version{};
    std::uint32_t channels = 2;
    std::span<const Parameter> parameters;
    bool supportsMono{};
    std::uint32_t editorWidth=1000,editorHeight=620;
    bool supportsSidechain{},acceptsMidi{};
    std::span<const MainBusLayout> mainBusLayouts{};
    bool producesMidi{}; // Optional host-facing MIDI output, independent of audio.
    bool supportsSysEx{}; // Opt in on the enabled MIDI input and output ports.
    bool supportsUmp{};   // Opt in to inline Universal MIDI Packets on supporting adapters.
};

// Optional non-audio format context. The adapter retains the native context
// until detach. A controller must never acquire audio-routing ownership.
class NativeHostContext {
public:
    virtual ~NativeHostContext()=default;
    virtual void nativeHostContext(std::string_view format,void* context,bool audioComponent)=0;
};

// Optional non-audio project identity supplied by a format's host extension.
// Zero denotes an unavailable identity; products retain their session selector.
class HostProjectContext {
public:
    virtual ~HostProjectContext()=default;
    virtual bool hostProjectContext(std::uint64_t)=0;
};

// Audio-thread output for changes made by an instrument (preset/program load).
// Backpressure is explicit; the producer retains an unaccepted value.
class ParameterOutput {
public:
    virtual ~ParameterOutput() = default;
    virtual bool parameterOutput(ParamId,double) noexcept = 0;
};
// Non-audio metadata for fixed host lanes whose target can change with a rack.
// Returning no override retains the descriptor's ordinary presentation.
struct ParameterPresentation {Parameter field;bool hidden{};};
class ParameterPresentationSource {
public:
    virtual ~ParameterPresentationSource()=default;
    virtual std::optional<ParameterPresentation> parameterPresentation(ParamId)const=0;
    virtual std::uint64_t parameterPresentationRevision()const noexcept=0;
};
class Processor {
public:
    virtual ~Processor() = default;
    virtual void attachParameters(const ParameterBank&) {} // construction, non-audio
    virtual void attachParameterOutput(ParameterOutput*) {} // construction
    // Construction only: seed product-owned host lanes before a host reads them.
    virtual void initializeParameters(ParameterBank&) {}
    // Audio thread, after an accepted host event. No locks or allocation.
    virtual void parameterChanged(ParamId,double)noexcept {}
    virtual void prepare(const ProcessSpec&) = 0;
    virtual void reset() noexcept = 0;
    virtual void process(AudioBlock<float>, const ParameterBank&, const Transport&,
                         std::span<const MidiEvent>) noexcept = 0;
    virtual void processSidechain(AudioBlock<float> block,const ParameterBank& p,const Transport& t,std::span<const MidiEvent> midi,AudioBlock<float>)noexcept{process(block,p,t,midi);}
    // A MIDI transform explicitly owns its output stream. Ordinary audio
    // processors retain transparent MIDI forwarding in Engine::process.
    virtual bool routesMidi()const noexcept{return false;}
    virtual void processMidi(AudioBlock<float> block,const ParameterBank& p,const Transport& t,
                             std::span<const MidiEvent> input,AudioBlock<float> side,midi::Output& output)noexcept{
        processSidechain(block,p,t,input,side);midi::forward(input,output);
    }
    virtual std::uint32_t latency() const noexcept { return 0; }
    virtual std::uint32_t tail() const noexcept { return 0; }
};

// Optional owning product-state codec. Calls are non-audio; products may require
// processing to be stopped. Format adapters must enforce that requirement.
class StateExtension {
public:
    virtual ~StateExtension() = default;
    virtual std::vector<std::byte> saveProductState(const ParameterBank&) const = 0;
    virtual bool loadProductState(std::span<const std::byte>, ParameterBank&) = 0;
    virtual bool stateRequiresStoppedProcessing() const noexcept { return true; }
    // Non-audio: false while a graph or an asset replacement is incomplete.
    virtual bool stateSnapshotReady() const { return true; }
    virtual std::uint64_t stateSnapshotRevision() const noexcept { return 0; }
};

class Engine;

struct ProgramAddress {
    unsigned bank{}, program{};
    friend bool operator==(const ProgramAddress&, const ProgramAddress&) = default;
};
struct ProgramSlot {
    unsigned number{};
    std::string name;
};
struct ProgramBank {
    unsigned number{};
    std::string name;
    std::vector<ProgramSlot> programs;
};
struct ProgramCatalog {
    std::vector<ProgramBank> banks;
    std::optional<ProgramAddress> active;
};

// Optional product-owned patch memory. Called on the serialized control thread.
// A recall must safely publish to the running processor or reject the request.
// Parameter-only patches can use Engine::editTransaction.
class ProgramMemory {
public:
    virtual ~ProgramMemory() = default;
    virtual ProgramCatalog programs(const ParameterBank&) const = 0;
    virtual bool recallProgram(Engine&, ProgramAddress) = 0;
};

class Engine : private ParameterOutput {
public:
    Engine(const PluginDescriptor&, std::unique_ptr<Processor>);
    ~Engine();
    void prepare(double sampleRate, std::uint32_t maxFrames, std::uint32_t channels = 0);
    void reset() noexcept;
    // Audio thread: host events must be ordered by sample offset. Invalid events
    // are ignored and processing continues without allocating or taking locks.
    void process(AudioBlock<float> block, std::span<const ParameterEvent> events = {},
                 const Transport& transport = {}, std::span<const MidiEvent> midi = {},AudioBlock<float> sidechain={},midi::Output* output=nullptr) noexcept;

    const PluginDescriptor& descriptor() const noexcept { return descriptor_; }
    ParameterBank& parameters() noexcept { return parameters_; }
    const ParameterBank& parameters() const noexcept { return parameters_; }
    const Processor& processor() const noexcept { return *processor_; }
    Processor& processor() noexcept { return *processor_; }
    bool prepared() const noexcept { return prepared_; }
    const ProcessSpec& spec() const noexcept { return spec_; }
    std::uint32_t channels() const noexcept { return spec_.channels; }
    bool stateRequiresStoppedProcessing() const noexcept {
        const auto* codec=dynamic_cast<const StateExtension*>(processor_.get());
        return !codec||codec->stateRequiresStoppedProcessing();
    }
    bool stateSaveRequiresStoppedProcessing() const noexcept {
        const auto* codec=dynamic_cast<const StateExtension*>(processor_.get());
        // The generic codec only reads parameter atomics. A product codec
        // must explicitly provide its own concurrent save/restore contract.
        return codec&&codec->stateRequiresStoppedProcessing();
    }

    // Main/UI thread producer, audio/flush thread consumer. A full queue is
    // reported to the caller; values are never silently dropped. Each active
    // parameter owns a reserved closing event, including under backpressure.
    static constexpr std::size_t maxConcurrentGestures = 32;
    bool edit(ParameterEdit) noexcept;
    // Single GUI producer: admit complete multi-parameter gestures and updates.
    // Duplicate/owned parameters reject the whole request without mutation.
    bool beginEdits(std::span<const ParamId>) noexcept;
    // Press-and-hold control: reserve both the release value and closing event.
    // endEdits/endAllEdits release it even when the ordinary edit queue is full.
    bool beginMomentary(ParamId,double pressedValue,double releasedValue=0) noexcept;
    bool editValues(std::span<const std::pair<ParamId,double>>) noexcept;
    bool endEdits(std::span<const ParamId>) noexcept;
    bool endAllEdits() noexcept;
    std::size_t activeGestures() const noexcept { return gestureCount_; }
    bool gestureActive(ParamId id) const noexcept;
    std::uint64_t stateGeneration() const noexcept { return stateGeneration_.load(std::memory_order_acquire); }
    // Atomic one-shot gesture; either all three events fit or none is published.
    bool editGesture(ParamId, double value) noexcept;
    // UI/control thread: publish a bounded set of complete gestures together.
    // On backpressure or invalid input neither bank nor queue changes.
    bool editTransaction(std::span<const std::pair<ParamId,double>>);
    bool pollEdit(ParameterEdit& edit) noexcept { return pollUiEdit(edit)||pollGenerated(edit); }
    // Adapters with separate GUI and audio delivery paths consume each queue
    // from exactly one thread; they must not also call the combined pollEdit.
    bool pollUiEdit(ParameterEdit& edit) noexcept { return edits_.pop(edit); }
    bool pollGenerated(ParameterEdit& edit) noexcept { return generated_.pop(edit); }
    std::vector<std::byte> saveState() const;
    // Non-audio. Active-processing restore requires a product codec with a
    // prepared publication contract; the generic CELL codec is stopped-only.
    bool loadState(std::span<const std::byte>);
    // Central non-audio routing, including framework snapshot transfers.
    bool executeProductCommand(std::span<const std::byte>);
    std::vector<std::byte> queryProduct(std::span<const std::byte>);
    // Serialized control-thread input for a view-owned instrument controller.
    // Audio consumes it at the next callback without locks or allocation.
    bool controllerMidi(const midi::ControlMessage&)noexcept;
    void releaseControllerMidi()noexcept;
    midi::PadInput* padInput()noexcept{return padInput_.get();} // control thread
    const midi::PadInput* padInput()const noexcept{return padInput_.get();}
private:
    bool parameterOutput(ParamId,double) noexcept override;
    std::string id_,name_,vendor_,version_;
    PluginDescriptor descriptor_;
    ParameterBank parameters_;
    std::unique_ptr<Processor> processor_;
    ProcessSpec spec_{};
    bool prepared_{};
    SpscQueue<ParameterEdit, 2048> edits_;
    SpscQueue<ParameterEdit, 256> generated_;
    std::array<ParamId,maxConcurrentGestures> gestureIds_{}; // GUI producer only
    std::array<double,maxConcurrentGestures> gestureReleaseValues_{};
    std::array<bool,maxConcurrentGestures> gestureMomentary_{};
    std::size_t gestureReservations_{};
    std::size_t gestureCount_{};
    std::atomic<std::uint64_t> stateGeneration_{};
    std::vector<MidiEvent> midiScratch_;
    std::unique_ptr<midi::ControlInput> controllerInput_;
    std::unique_ptr<midi::PadInput> padInput_;
    std::vector<MidiEvent> hardwareScratch_;
    std::vector<MidiEvent> controllerScratch_;
    std::mutex snapshotsMutex_;
    std::unique_ptr<state::SnapshotServer> snapshots_;
};

namespace plugin {
const PluginDescriptor& descriptor();
std::unique_ptr<Processor> createProcessor();
}

} // namespace cell
