// SPDX-License-Identifier: Apache-2.0
#include "../src/formats/auv3_automation.hpp"
#include "../src/formats/auv3_layouts.hpp"
#include "allocation_watch.hpp"
#include <cell/midi_delivery.hpp>
#include <array>
#include <iostream>

namespace {
using cell::auv3::AutomationEvent;
const std::array<cell::Parameter, 2> fields{{
    {17, "left", "Left", "", 0, 1, 0},
    {18, "right", "Right", "", 0, 1, 1},
}};
constexpr double sampleRate = 48000;
constexpr std::uint32_t maxFrames = 16;
constexpr std::size_t maxEvents = 4096 + fields.size();

void check(bool condition, const char* message) {
    if (!condition) throw std::runtime_error(message);
}

class Probe final : public cell::Processor {
public:
    void prepare(const cell::ProcessSpec&) override {}
    void reset() noexcept override {}
    void process(cell::AudioBlock<float> audio, const cell::ParameterBank& bank,
                 const cell::Transport& clock, std::span<const cell::MidiEvent> midi) noexcept override {
        std::fill_n(audio.channel(0), audio.frames, static_cast<float>(bank.get(17)));
        std::fill_n(audio.channel(1), audio.frames, static_cast<float>(bank.get(18)));
        clocks[audio.offset] = clock;
        for (auto message : midi) {
            midiInRange &= message.frame < audio.frames;
            message.frame += audio.offset;
            if (midiCount < received.size()) received[midiCount] = message;
            ++midiCount;
        }
    }
    void processSidechain(cell::AudioBlock<float> audio, const cell::ParameterBank& bank,
                          const cell::Transport& clock, std::span<const cell::MidiEvent> midi,
                          cell::AudioBlock<float> sidechain) noexcept override {
        process(audio, bank, clock, midi);
        if (!sidechain.channelCount) return;
        for (unsigned channel = 0; channel < audio.channelCount; ++channel) {
            const auto* key = sidechain.channel(std::min(channel, sidechain.channelCount - 1));
            for (unsigned frame = 0; frame < audio.frames; ++frame)
                audio.channel(channel)[frame] += key[frame];
        }
    }
    bool routesMidi() const noexcept override { return generateMidi; }
    void processMidi(cell::AudioBlock<float> audio, const cell::ParameterBank& bank,
                     const cell::Transport& clock, std::span<const cell::MidiEvent> midi,
                     cell::AudioBlock<float> sidechain, cell::midi::Output& output) noexcept override {
        processSidechain(audio, bank, clock, midi, sidechain);
        for (auto event : midi) {
            if ((event.data[0] & 0xf0) == 0x90) event.data[1] = 72;
            if (!output.push(event)) return;
        }
        if (audio.offset == 4) output.push({invalidOffset ? audio.frames : 0, {0xc1, 9, 0}, 2});
    }
    std::array<cell::Transport, maxFrames> clocks{};
    std::array<cell::MidiEvent, cell::midi::eventCapacity> received{};
    std::size_t midiCount{};
    bool midiInRange = true;
    bool generateMidi{}, invalidOffset{};
};

class Fixture {
public:
    Fixture()
        : engine({.id = "cell.test.auv3", .name = "AUv3 automation", .vendor = "Cell", .version = "1",
                  .channels = 2, .parameters = fields, .acceptsMidi = true},
                 std::make_unique<Probe>()), automation(fields.size(), maxEvents, cell::midi::eventCapacity) {
        engine.prepare(sampleRate, maxFrames, 2);
    }

    void render(std::uint32_t frames, std::span<const AutomationEvent> events = {},
                cell::AudioBlock<float> sidechain = {}, std::span<const cell::MidiEvent> midi = {},
                cell::midi::Output* output = nullptr) {
        cell::Transport clock;
        clock.playing = true;
        clock.tempo = 137.5;
        clock.samplePosition = -960;
        clock.samplePositionValid = true;
        clock.beatPosition = -0.5;
        clock.beatPositionValid = true;
        const auto block = buffer.block().slice(0, frames);
        cell_test::allocations = cell_test::deletions = 0;
        cell_test::audio = true;
        automation.process(engine, block, events, clock, sidechain, midi, output);
        cell_test::audio = false;
        check(!cell_test::allocations && !cell_test::deletions, "AUv3 render allocated or freed memory");
    }

    void expect(unsigned channel, std::initializer_list<float> values) {
        check(std::equal(values.begin(), values.end(), buffer.block().channel(channel)),
              "Parameter values landed on the wrong samples");
    }

    cell::Engine engine;
    cell::auv3::Automation automation;
    cell::AudioBuffer<float> buffer{2, maxFrames};
};

void test() {
    Fixture fixture;
    const std::array overlap{AutomationEvent{2, 0, 1, 8}, AutomationEvent{3, 1, 0, 4}};
    fixture.render(5, overlap);
    fixture.expect(0, {0, 0, 0, 0.125f, 0.25f});
    fixture.expect(1, {1, 1, 1, 1, 0.75f});
    const auto& clock = static_cast<Probe&>(fixture.engine.processor()).clocks[4];
    check(clock.samplePositionValid && clock.samplePosition == -956 && clock.beatPositionValid &&
        std::abs(clock.beatPosition - (-0.5 + 4 / sampleRate * 137.5 / 60)) < 1e-12,
        "Sample and beat positions drifted during a ramp");

    fixture.render(6);
    fixture.expect(0, {0.375f, 0.5f, 0.625f, 0.75f, 0.875f, 1});
    fixture.expect(1, {0.5f, 0.25f, 0, 0, 0, 0});

    const std::array interrupt{AutomationEvent{0, 0, 0.25, 0}, AutomationEvent{0, 0, 0.75, 4},
                               AutomationEvent{2, 0, 0.5, 0}};
    fixture.render(5, interrupt);
    fixture.expect(0, {0.25f, 0.375f, 0.5f, 0.5f, 0.5f});

    const std::array rise{AutomationEvent{0, 0, 1, 4}};
    fixture.render(2, rise);
    fixture.expect(0, {0.5f, 0.625f});
    const std::array reverse{AutomationEvent{0, 0, 0, 3}};
    fixture.render(4, reverse);
    fixture.expect(0, {0.75f, 0.5f, 0.25f, 0});

    fixture.render(2, rise);
    fixture.automation.reset();
    fixture.render(3);
    fixture.expect(0, {0.25f, 0.25f, 0.25f});

    fixture.render(2, rise);
    std::array<AutomationEvent, maxEvents> dense;
    dense.fill({0, 1, 0.5, 0});
    fixture.render(2, dense);
    fixture.expect(0, {0.625f, 0.8125f});
    fixture.expect(1, {0.5f, 0.5f});

    auto discrete = fields[0];
    discrete.step = 1;
    auto bypass = fields[0];
    bypass.bypass = true;
    auto manual = fields[0];
    manual.automatable = false;
    check(cell::auv3::canRamp(fields[0]) && !cell::auv3::canRamp(discrete) &&
        !cell::auv3::canRamp(bypass) && !cell::auv3::canRamp(manual), "Incorrect ramp capability");
}

void testSidechain() {
    Fixture fixture;
    cell::AudioBuffer<float> stereo(2, 6), mono(1, 3);
    auto key = stereo.block();
    for (unsigned frame = 0; frame < key.frames; ++frame) {
        key.channel(0)[frame] = static_cast<float>(frame) / 8;
        key.channel(1)[frame] = -static_cast<float>(frame) / 8;
    }
    const std::array ramp{AutomationEvent{1, 0, 1, 4}};
    fixture.render(6, ramp, key);
    fixture.expect(0, {0, 0.125f, 0.5f, 0.875f, 1.25f, 1.625f});
    fixture.expect(1, {1, 0.875f, 0.75f, 0.625f, 0.5f, 0.375f});

    key = mono.block();
    key.channel(0)[0] = 0.25f;
    key.channel(0)[1] = 0.5f;
    key.channel(0)[2] = 0.75f;
    fixture.render(3, {}, key);
    fixture.expect(0, {1.25f, 1.5f, 1.75f});
    fixture.expect(1, {1.25f, 1.5f, 1.75f});

    fixture.render(3);
    fixture.expect(0, {1, 1, 1});
    fixture.expect(1, {1, 1, 1});
}

void testMidi() {
    Fixture fixture;
    const std::array automation{AutomationEvent{3, 0, 1, 4}, AutomationEvent{9, 1, 0.5, 0}};
    const std::array midi{cell::MidiEvent{0, {0x91, 60, 100}, 3}, cell::MidiEvent{2, {0xc1, 10, 0}, 2},
                         cell::MidiEvent{3, {0xb1, 74, 45}, 3}, cell::MidiEvent{3, {0xd1, 75, 0}, 2},
                         cell::MidiEvent{4, {0xe1, 0, 64}, 3}, cell::MidiEvent{7, {0x81, 60, 0}, 3},
                         cell::MidiEvent{15, {0x90, 63, 0}, 3}};
    fixture.render(maxFrames, automation, {}, midi);
    auto& probe = static_cast<Probe&>(fixture.engine.processor());
    check(probe.midiInRange && probe.midiCount == midi.size(), "MIDI messages were lost at automation boundaries");
    for (std::size_t i = 0; i < midi.size(); ++i) {
        const auto& observed = probe.received[i];
        check(observed.frame == midi[i].frame && observed.size == midi[i].size && observed.data == midi[i].data,
              "MIDI timestamp, order or payload changed across an automation split");
    }

    probe.midiCount = 0;
    std::array<cell::MidiEvent, cell::midi::eventCapacity> dense;
    dense.fill({3, {0xb0, 7, 127}, 3});
    fixture.render(maxFrames, automation, {}, dense);
    check(probe.midiInRange && probe.midiCount == dense.size() && probe.received.back().frame == 3,
          "MIDI lost at the supported event limit");
}

void testMidiOutput() {
    struct Sink final : cell::midi::Output {
        bool push(cell::MidiEvent event) noexcept override {
            if (count == events.size()) return false;
            events[count++] = event;
            return true;
        }
        std::array<cell::MidiEvent, 64> events{};
        std::size_t count{};
    } sink;
    cell::midi::Delivery delivery;
    Fixture fixture;
    auto& probe = static_cast<Probe&>(fixture.engine.processor());
    const std::array automation{AutomationEvent{0, 0, 0, 0}, AutomationEvent{3, 0, 1, 4}};
    const std::array input{cell::MidiEvent{0, {0x91, 60, 100}, 3}, cell::MidiEvent{3, {0xb1, 7, 45}, 3},
                          cell::MidiEvent{15, {0x81, 60, 0}, 3}};
    auto render = [&] {
        sink.count = 0;
        delivery.begin(&sink, maxFrames);
        fixture.render(maxFrames, automation, {}, input, &delivery);
    };
    render();
    check(sink.count == input.size() && sink.events[1].frame == 3 && sink.events[2].frame == 15 &&
          sink.events[0].data == input[0].data, "Transparent MIDI lost its original offsets");

    probe.generateMidi = true;
    render();
    check(sink.count == 4 && sink.events[0].data[1] == 72 && sink.events[2].frame == 4 &&
          sink.events[2].size == 2 && sink.events[2].data[0] == 0xc1 && sink.events[3].frame == 15,
          "Generated or transformed MIDI lost timing across render segments");

    probe.invalidOffset = true;
    render();
    check(sink.count == 2 && delivery.pending(), "An out-of-segment MIDI event reached the host");
    probe.invalidOffset = false;
    render();
    check(sink.count == 52 && !delivery.pending() && sink.events[0].data[1] == 64 &&
          sink.events[47].data == std::array<std::uint8_t, 3>{0xbf, 123, 0} &&
          sink.events[48].data[0] == 0x91 && sink.events[50].frame == 4,
          "MIDI recovery did not finish before subsequent generated output");
}

void testLayoutMapping() {
    using cell::AudioLayout;
    using cell::MainBusLayout;
    using namespace cell::auv3;
    cell::PluginDescriptor legacy;
    legacy.supportsMono = true;
    check(defaultMainLayout(legacy) == AudioLayout::stereo && supportsMainLayout(legacy, AudioLayout::mono) &&
          !supportsMainLayout(legacy, AudioLayout::surround51), "Legacy AUv3 layout changed");

    const std::array layouts{
        MainBusLayout{AudioLayout::surround51, AudioLayout::surround51},
        MainBusLayout{AudioLayout::surround714, AudioLayout::surround916},
        MainBusLayout{AudioLayout::surround916, AudioLayout::surround916},
    };
    auto spatial = legacy;
    spatial.channels = 16;
    spatial.mainBusLayouts = layouts;
    check(defaultMainLayout(spatial) == AudioLayout::surround916 &&
          supportsMainLayout(spatial, AudioLayout::surround51) &&
          !supportsMainLayout(spatial, AudioLayout::surround714) &&
          !supportsMainLayout(spatial, AudioLayout::mono), "AUv3 advertised an undeclared symmetric layout");
    const std::array outputs{MainBusLayout{AudioLayout::stereo, AudioLayout::surround714},
                             MainBusLayout{AudioLayout::stereo, AudioLayout::mono}};
    spatial.channels = 12;
    spatial.mainBusLayouts = outputs;
    check(!defaultMainLayout(spatial) && defaultMainLayout(spatial, true) == AudioLayout::surround714 &&
          supportsMainLayout(spatial, AudioLayout::mono, true) && !supportsMainLayout(spatial, AudioLayout::stereo, true),
          "AUv3 instrument layout selection did not use the declared output roles");

    constexpr std::array cinema51{Speaker::center, Speaker::left, Speaker::right,
        Speaker::rearLeft, Speaker::rearRight, Speaker::lfe};
    const auto five = channelOrder(AudioLayout::surround51, cinema51);
    const std::array<unsigned, 6> cinemaOrder{1, 2, 0, 5, 3, 4};
    check(five && std::equal(cinemaOrder.begin(), cinemaOrder.end(), five->begin()), "Incorrect 5.1 role order");

    std::array atmos916{
        Speaker::left, Speaker::right, Speaker::center, Speaker::lfe,
        Speaker::sideLeft, Speaker::sideRight, Speaker::rearLeft, Speaker::rearRight,
        Speaker::wideLeft, Speaker::wideRight, Speaker::topFrontLeft, Speaker::topFrontRight,
        Speaker::topMiddleLeft, Speaker::topMiddleRight, Speaker::topRearLeft, Speaker::topRearRight,
    };
    const auto sixteen = channelOrder(AudioLayout::surround916, atmos916);
    check(sixteen == ChannelOrder{0, 1, 2, 3, 6, 7, 4, 5, 10, 11, 14, 15, 12, 13, 8, 9},
          "Incorrect 9.1.6 role order");
    atmos916[8] = Speaker::rearLeft;
    check(!channelOrder(AudioLayout::surround916, atmos916) &&
          !channelOrder(AudioLayout::surround712, std::span(atmos916).first(10)),
          "AUv3 accepted a channel count without the correct speaker roles");
}

void testMidiOnly() {
    struct MidiProcessor final : cell::Processor {
        void prepare(const cell::ProcessSpec& spec) override {
            check(spec.channels == 0, "A MIDI-only processor was prepared with audio channels");
        }
        void reset() noexcept override {}
        void process(cell::AudioBlock<float>, const cell::ParameterBank&, const cell::Transport&,
                     std::span<const cell::MidiEvent>) noexcept override {}
        bool routesMidi() const noexcept override { return true; }
        void processMidi(cell::AudioBlock<float> audio, const cell::ParameterBank& parameters, const cell::Transport&,
                         std::span<const cell::MidiEvent> input, cell::AudioBlock<float> sidechain,
                         cell::midi::Output& output) noexcept override {
            valid &= !audio.channels && !audio.channelCount && audio.frames > 0 && !sidechain.channelCount;
            if (input.empty() && audio.offset == 0) output.push({0, {0xb0, 7, 100}, 3});
            for (auto event : input) {
                event.data[1] += static_cast<std::uint8_t>(parameters.get(17));
                if (!output.push(event)) break;
            }
        }
        bool valid = true;
    };
    struct Sink final : cell::midi::Output {
        bool push(cell::MidiEvent event) noexcept override {
            if (count == events.size()) return false;
            events[count++] = event;
            return true;
        }
        std::array<cell::MidiEvent, 4> events{};
        std::size_t count{};
    } sink;
    cell::PluginDescriptor descriptor{.id = "cell.test.midi-fx", .name = "MIDI Effect", .vendor = "Cell",
        .version = "1", .channels = 0, .parameters = fields, .acceptsMidi = true, .producesMidi = true};
    auto processor = std::make_unique<MidiProcessor>();
    const auto* probe = processor.get();
    cell::Engine engine(descriptor, std::move(processor));
    engine.prepare(sampleRate, maxFrames);
    cell::auv3::Automation automation(fields.size(), maxEvents, cell::midi::eventCapacity);
    const std::array parameters{AutomationEvent{8, 0, 1, 0}};
    const std::array input{cell::MidiEvent{2, {0x90, 60, 90}, 3}, cell::MidiEvent{10, {0x91, 62, 95}, 3}};
    const cell::AudioBlock<float> audio{.frames = maxFrames};
    cell_test::allocations = cell_test::deletions = 0;
    cell_test::audio = true;
    automation.process(engine, audio, parameters, {}, {}, input, &sink);
    automation.process(engine, audio, {}, {}, {}, {}, &sink);
    cell_test::audio = false;
    check(!cell_test::allocations && !cell_test::deletions, "MIDI-only render allocated memory");
    check(probe->valid && sink.count == 3 && sink.events[0].frame == 2 && sink.events[0].data[1] == 60 &&
          sink.events[1].frame == 10 && sink.events[1].data[1] == 63 &&
          sink.events[2].frame == 0 && sink.events[2].data[0] == 0xb0,
          "MIDI-only processing lost timing or generated output");

    descriptor.producesMidi = false;
    bool rejected{};
    try {
        cell::Engine invalid(descriptor, std::make_unique<MidiProcessor>());
    } catch (const std::invalid_argument&) {
        rejected = true;
    }
    check(rejected, "A zero-channel descriptor with no MIDI output was accepted");
}
} // namespace

int main() {
    try {
        test();
        testSidechain();
        testMidi();
        testMidiOutput();
        testLayoutMapping();
        testMidiOnly();
        std::cout << "AUv3 channel order, automation, sidechain and zero-channel MIDI checks passed.\n";
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
