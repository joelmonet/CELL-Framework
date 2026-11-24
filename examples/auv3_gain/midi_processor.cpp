// Original Cell Framework implementation; see LICENSE.
#include "midi_fixture.hpp"
#include <cell/plugin.hpp>

namespace {
using namespace cell::auv3_example;
const std::array<cell::Parameter, 1> fields{{{transpose, "transpose", "Transpose", "", 0, 12, 0, 1}}};

class MidiProbe final : public cell::Processor {
public:
    void prepare(const cell::ProcessSpec& spec) override {
        if (spec.channels != 0 || spec.sampleRate != midiSampleRate || spec.maxFrames != midiFrames)
            throw std::runtime_error("MIDI processor received the wrong sample clock or audio channel count");
    }
    void reset() noexcept override {}
    void process(cell::AudioBlock<float>, const cell::ParameterBank&, const cell::Transport&,
                 std::span<const cell::MidiEvent>) noexcept override {}
    bool routesMidi() const noexcept override { return true; }
    void processMidi(cell::AudioBlock<float> audio, const cell::ParameterBank& bank, const cell::Transport&,
                     std::span<const cell::MidiEvent> input, cell::AudioBlock<float> sidechain,
                     cell::midi::Output& output) noexcept override {
        if (audio.channelCount || audio.channels || sidechain.channelCount || !audio.frames) return;
        if (input.empty() && audio.offset == 0) output.push({0, {0xb0, 7, 100}, 3});
        for (auto event : input) {
            const auto kind = event.data[0] & 0xf0;
            if (kind == 0x80 || kind == 0x90) {
                const auto note = static_cast<unsigned>(event.data[1]) + static_cast<unsigned>(bank.get(transpose));
                event.data[1] = static_cast<std::uint8_t>(std::min(note, 127u));
            }
            if (!output.push(event)) break;
        }
    }
};
} // namespace

namespace cell::plugin {
const PluginDescriptor& descriptor() {
    static const PluginDescriptor result{.id = "cell.example.auv3-midi-probe", .name = "Cell MIDI Probe", .vendor = "Cell",
        .version = "1", .channels = 0, .parameters = fields, .acceptsMidi = true, .producesMidi = true};
    return result;
}
std::unique_ptr<Processor> createProcessor() { return std::make_unique<MidiProbe>(); }
} // namespace cell::plugin
