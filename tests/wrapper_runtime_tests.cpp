#include <cell/wrapper.hpp>
#include "allocation_watch.hpp"
#include <atomic>
#include <iostream>
#include <limits>
#include <thread>

namespace {
using cell::wrapper::Block;
using cell::wrapper::Phase;
using cell::wrapper::Result;
using cell::wrapper::Runtime;
constexpr cell::ParamId gainId = 17;
const std::array<cell::Parameter, 1> parameters{{{gainId, "gain", "Gain", "", 0, 4, 1}}};
const cell::PluginDescriptor descriptor{"org.cell.wrapper.test", "Wrapper Test", "Cell", "1", 2, parameters, true, 800, 500, true, true};
void check(bool condition, const char* message) { if (!condition) throw std::runtime_error(message); }
void ok(Result result, const char* message) { check(result == Result::ok, message); }

struct Probe : cell::Processor {
    cell::ProcessSpec prepared;
    struct Note { cell::MidiEvent event; std::int64_t position; };
    std::array<Note, 4096> notes{};
    std::array<cell::Transport, 4096> calls{};
    std::size_t noteCount{}, callCount{};
    std::atomic<bool> wait{}, entered{}, release{};
    bool throwPrepare{};
    void prepare(const cell::ProcessSpec& value) override {
        if (throwPrepare) throw std::runtime_error("deliberate preparation failure");
        prepared = value;
    }
    void reset() noexcept override { noteCount = callCount = 0; }
    void process(cell::AudioBlock<float> block, const cell::ParameterBank& bank,
        const cell::Transport& transport, std::span<const cell::MidiEvent> midi) noexcept override {
        if (wait.load()) { entered.store(true); while (!release.load()) std::this_thread::yield(); }
        if (callCount < calls.size()) calls[callCount++] = transport;
        for (const auto& event : midi) if (noteCount < notes.size()) notes[noteCount++] = {event, transport.samplePosition + event.frame};
        block.multiply(float(bank.get(gainId)));
    }
    void processSidechain(cell::AudioBlock<float> block, const cell::ParameterBank& bank,
        const cell::Transport& transport, std::span<const cell::MidiEvent> midi, cell::AudioBlock<float> side) noexcept override {
        for (unsigned channel = 0; channel < block.channelCount; ++channel)
            if (side.channelCount) for (unsigned frame = 0; frame < block.frames; ++frame)
                block.channel(channel)[frame] += side.channel(channel % side.channelCount)[frame];
        process(block, bank, transport, midi);
    }
};

void chunkingAndTiming() {
    auto processor = std::make_unique<Probe>(); auto* probe = processor.get();
    Runtime runtime(descriptor, std::move(processor));
    check(runtime.phase() == Phase::unprepared && runtime.start() == Result::wrongPhase, "unprepared lifecycle");
    ok(runtime.configure({48000, 7, 2}), "prepare chunks");
    ok(runtime.activate(), "activate"); ok(runtime.start(), "start");
    check(runtime.configure({48000, 64, 2}) == Result::wrongPhase, "active preparation accepted");
    std::array<float, 23> left, right, key; left.fill(1); right.fill(2); key.fill(.5f);
    const std::array<const float*, 2> inputs{left.data(), right.data()};
    const std::array<float*, 2> outputs{right.data(), left.data()}; // cross-channel in-place aliases
    const std::array<const float*, 1> side{key.data()};
    const std::array<cell::ParameterEvent, 6> events{{{23, gainId, 1}, {7, gainId, .5}, {0, gainId, 2}, {14, gainId, 0}, {7, gainId, 1}, {13, gainId, 3}}};
    const std::array<cell::MidiEvent, 4> midi{{{22, {0x80, 60, 0}, 3}, {7, {0x90, 60, 100}, 3}, {0, {0xc0, 8, 0}, 2}, {14, {0xd0, 20, 0}, 2}}};
    cell::Transport transport; transport.samplePosition = 500; transport.beatPosition = 8; transport.tempo = 90;
    transport.playing = transport.recording = transport.looping = transport.offline = true;
    transport.tempoValid=false;
    cell_test::allocations = cell_test::deletions = 0; cell_test::audio = true;
    const auto rendered = runtime.process({23, inputs, outputs, side, events, midi, transport});
    cell_test::audio = false;
    ok(rendered, "chunked render");
    check(cell_test::allocations == 0 && cell_test::deletions == 0, "wrapper allocated or destroyed on audio thread");
    for (unsigned frame = 0; frame < 23; ++frame) {
        const auto gain = frame < 7 ? 2.f : frame < 13 ? 1.f : frame < 14 ? 3.f : 0.f;
        check(right[frame] == 1.5f * gain && left[frame] == 2.5f * gain, "chunked automation or aliased sidechain copy");
    }
    check(runtime.engine().parameters().get(gainId) == 1, "block-end event did not commit");
    check(probe->prepared.maxFrames == 7 && probe->noteCount == 4, "chunk preparation or MIDI count");
    for (unsigned n = 0; n < 4; ++n) check(probe->notes[n].position == 500 + std::array{0, 7, 14, 22}[n], "MIDI rebased more than once");
    for (std::size_t n = 0; n < probe->callCount; ++n) {
        const auto& call = probe->calls[n];
        check(std::abs(call.beatPosition - (8 + double(call.samplePosition - 500) / 48000 * 1.5)) < 1e-12, "transport beats rebased incorrectly");
        check(call.playing && call.recording && call.looping && call.offline, "transport flags lost");
        check(!call.tempoValid,"Unavailable host tempo became valid during runtime chunking/automation segmentation");
    }
    ok(runtime.stop(), "stop"); ok(runtime.deactivate(), "deactivate");
    ok(runtime.configure({96000, 1u << 20, 1}), "mono reconfigure");
    check(probe->prepared.maxFrames == cell::wrapper::Limits::renderFrames && probe->prepared.channels == 1, "bounded render quantum");
}

void overflowAndState() {
    auto processor = std::make_unique<Probe>(); auto* probe = processor.get();
    Runtime runtime(descriptor, std::move(processor));
    ok(runtime.configure({48000, 32, 2}), "prepare overflow probe");
    std::vector<std::byte> state;
    ok(runtime.saveState(state), "save stopped state");
    check(runtime.engine().stateRequiresStoppedProcessing(), "generic state codec must require stop");
    ok(runtime.activate(), "activate overflow probe"); ok(runtime.start(), "start overflow probe");
    std::vector<std::byte> activeSave;
    ok(runtime.saveState(activeSave), "atomic generic state save while processing");
    check(activeSave == state, "active state save changed encoding");
    check(runtime.loadState(state) == Result::wrongPhase, "unsafe active generic state restore");
    std::array<float, 32> left, right; left.fill(1); right.fill(1);
    const std::array<float*, 2> outputs{left.data(), right.data()};
    std::array<cell::ParameterEvent, cell::wrapper::Limits::parameters + 1> tooMany;
    tooMany.fill({0, gainId, 2});
    check(runtime.process({32, {}, outputs, {}, tooMany}) == Result::eventOverflow, "automation overflow accepted");
    check(probe->callCount == 0 && left[0] == 0 && runtime.engine().parameters().get(gainId) == 1, "overflow partially processed");
    const std::array<cell::MidiEvent, 1> note{{{0, {0x90, 60, 100}, 3}}};
    ok(runtime.process({32, {}, outputs, {}, {}, note}), "panic recovery block");
    check(probe->noteCount == 33, "MIDI recovery missing");
    for (unsigned n = 0; n < 32; ++n)
        check(probe->notes[n].event.data[0] == (0xb0 | (n / 2)) && probe->notes[n].event.data[1] == (n % 2 ? 123 : 120), "MIDI panic order");
    check(probe->notes[32].event.data[0] == 0x90, "new note must follow panic");
    std::array<cell::MidiEvent, 1> badMidi{{{0, {0xc0, 10, 0}, 3}}};
    check(runtime.process({32, {}, outputs, {}, {}, badMidi}) == Result::invalidEvent, "invalid short MIDI length");
    const std::array<cell::ParameterEvent, 1> boundary{{{0, gainId, .25}}};
    ok(runtime.process({0, {}, outputs, {}, boundary}), "zero-frame parameter flush");
    check(runtime.engine().parameters().get(gainId) == .25, "flush value lost");
    ok(runtime.stop(), "stop state probe");
    runtime.engine().parameters().set(gainId, 3);
    ok(runtime.loadState(state), "stopped restore");
    check(runtime.engine().parameters().get(gainId) == 1, "state restored wrong value");
    auto bad = state; bad[0] ^= std::byte(1);
    check(runtime.loadState(bad) == Result::stateRejected && runtime.engine().parameters().get(gainId) == 1, "malformed state partially applied");
    // Both fixtures pass the header and length checks. The first record
    // would change the live gain if restore committed before validation ended.
    runtime.engine().parameters().set(gainId, 3);
    const auto before = runtime.engine().saveState();
    const auto generation = runtime.engine().stateGeneration();
    const auto requireRejected = [&](std::span<const std::byte> bytes) {
        check(runtime.loadState(bytes) == Result::stateRejected, "Invalid trailing parameter was accepted");
        check(runtime.engine().saveState() == before && runtime.engine().stateGeneration() == generation,
              "Rejected state changed parameters or the state generation");
    };

    auto withTrailingParameter = state;
    withTrailingParameter[12] = std::byte{2}; // Two records in the CELL v1 header.
    cell::ByteWriter duplicate;
    duplicate.u32(gainId);
    duplicate.f64(2);
    withTrailingParameter.insert(withTrailingParameter.end(), duplicate.bytes.begin(), duplicate.bytes.end());
    requireRejected(withTrailingParameter);

    withTrailingParameter.resize(state.size());
    cell::ByteWriter nonfinite;
    nonfinite.u32(gainId + 1);
    nonfinite.f64(std::numeric_limits<double>::quiet_NaN());
    withTrailingParameter.insert(withTrailingParameter.end(), nonfinite.bytes.begin(), nonfinite.bytes.end());
    requireRejected(withTrailingParameter);

    ok(runtime.loadState(state), "restore following rejected states");
    check(runtime.engine().parameters().get(gainId) == 1 && runtime.engine().stateGeneration() == generation + 1,
          "Valid retry failed to restore gain or publish a state generation");
    ok(runtime.deactivate(), "deactivate state probe");
    probe->throwPrepare = true;
    check(runtime.configure({48000, 64, 2}) == Result::processorFailure && runtime.phase() == Phase::unprepared, "failed preparation left runnable instance");
    probe->throwPrepare = false;
    ok(runtime.configure({48000, 64, 2}), "recovery from failed preparation");
}

void lifecycleContention() {
    auto processor = std::make_unique<Probe>(); auto* probe = processor.get();
    Runtime runtime(descriptor, std::move(processor));
    ok(runtime.configure({48000, 32, 2}), "prepare race probe");
    std::vector<std::byte> state;ok(runtime.saveState(state),"save race fixture");
    ok(runtime.activate(), "activate race probe"); ok(runtime.start(), "start race probe");
    std::array<float, 32> left{}, right{};
    const std::array<float*, 2> outputs{left.data(), right.data()};
    probe->wait.store(true);
    Result rendered = Result::processorFailure;
    std::thread audio([&] { rendered = runtime.process({32, {}, outputs}); });
    while (!probe->entered.load()) std::this_thread::yield();
    // Never throw before joining the deliberately blocked audio thread.
    const auto stopped = runtime.stop();
    const auto reset = runtime.reset();
    const auto restarted = runtime.start();
    const auto deactivated = runtime.deactivate();
    const auto configured = runtime.configure({96000, 64, 1});
    const auto restored = runtime.loadStateAtIdle(state);
    probe->release.store(true); audio.join();
    ok(rendered, "in-flight render did not finish");
    ok(stopped, "stop admission during render"); ok(deactivated, "deactivate during render");
    check(reset == Result::busy && restarted == Result::busy && configured == Result::busy && restored == Result::busy, "lifecycle modified live processor");
    ok(runtime.configure({96000, 64, 1}), "configure after reader retires");
    for (unsigned n = 0; n < 100; ++n) {
        ok(runtime.activate(), "repeated activate"); ok(runtime.start(), "repeated start");
        ok(runtime.stop(), "repeated stop"); ok(runtime.deactivate(), "repeated deactivate");
    }
}

void stateAndTimedPackets() {
    Runtime runtime(descriptor,std::make_unique<Probe>());
    ok(runtime.configure({48000,4,2}),"prepare state/timeline probe");
    std::vector<std::byte> state;ok(runtime.saveState(state),"save state/timeline fixture");
    ok(runtime.activate(),"activate state/timeline probe");ok(runtime.start(),"start state/timeline probe");
    cell::wrapper::ParameterTimeline timeline;
    const std::array<cell::wrapper::ParameterTimeline::Value,1> before{{{gainId,3}}},after{{{gainId,.5}}};
    auto old=timeline.publish(runtime.engine().parameters(),before);check(bool(old),"publish pre-restore automation");
    ok(runtime.loadStateAtIdle(state,&timeline),"suspend idle render admission for restore");
    check(runtime.phase()==Phase::processing,"idle restore lost processing phase");
    std::array<float,8> left,right;left.fill(1);right.fill(1);
    const std::array<const float*,2> input{left.data(),right.data()};const std::array<float*,2> output{left.data(),right.data()};
    ok(runtime.process({8,input,output,{},{},{},{},&timeline,*old}),"consume cancelled token");
    check(left[0]==1&&runtime.engine().parameters().get(gainId)==1,"old timed packet overwrote restored state");
    auto newer=timeline.publish(runtime.engine().parameters(),after);check(bool(newer),"publish post-restore automation");
    ok(runtime.process({8,input,output,{},{},{},{},&timeline,*newer}),"adopt post-restore token");
    check(left[0]==.5&&left[7]==.5,"timed values were not applied once before all chunks");
    auto pending=timeline.publish(runtime.engine().parameters(),before);check(bool(pending),"publish retained automation");
    auto bad=state;bad[0]^=std::byte(1);
    check(runtime.loadStateAtIdle(bad,&timeline)==Result::stateRejected,"invalid idle state accepted");
    ok(runtime.process({8,input,output,{},{},{},{},&timeline,*pending}),"retain automation after failed restore");
    check(left[0]==1.5&&runtime.engine().parameters().get(gainId)==3,"failed state restore cancelled accepted automation");
}

struct StateProbe final : Probe,cell::StateExtension {
    bool live{};
    std::vector<std::byte> saveProductState(const cell::ParameterBank& bank) const override {
        cell::ByteWriter writer;writer.u32(77);writer.f64(bank.get(gainId));return std::move(writer.bytes);
    }
    bool loadProductState(std::span<const std::byte> bytes,cell::ParameterBank& bank) override {
        cell::ByteReader reader(bytes);std::uint32_t magic{};double value{};
        return reader.u32(magic)&&magic==77&&reader.f64(value)&&!reader.remaining()&&bank.set(gainId,value);
    }
    bool stateRequiresStoppedProcessing() const noexcept override{return !live;}
};

void productStateContracts() {
    for(bool live:{false,true}) {
        auto processor=std::make_unique<StateProbe>();processor->live=live;
        Runtime runtime(descriptor,std::move(processor));ok(runtime.configure({48000,32,2}),"prepare product codec");
        std::vector<std::byte> state;ok(runtime.saveState(state),"save product codec");
        ok(runtime.activate(),"activate product codec");ok(runtime.start(),"start product codec");
        const auto saved=runtime.saveState(state),loaded=runtime.loadState(state);
        check(saved==(live?Result::ok:Result::wrongPhase)&&loaded==(live?Result::ok:Result::wrongPhase),"custom codec stop contract ignored");
        ok(runtime.stop(),"stop product codec");ok(runtime.loadState(state),"stopped product codec restore");
    }
}
}

int main() {
    try { chunkingAndTiming(); overflowAndState(); lifecycleContention(); stateAndTimedPackets(); productStateContracts();
        std::cout << "PASS: wrapper chunking, timing, state, overflow, no audio allocation and lifecycle contention\n";
    } catch (const std::exception& error) { std::cerr << error.what() << '\n'; return 1; }
}
