// Original Cell Framework implementation; see LICENSE.
#include "instrument_fixture.hpp"
#include <cell/plugin.hpp>
#include <cell/synthesis.hpp>

namespace {
using namespace cell::auv3_example;
const std::array<cell::Parameter, 1> fields{{{instrumentLevel, "level", "Level", "", 0, 1, 0.25}}};

// A single reference voice with immediate note edges makes render timing measurable.
class InstrumentProbe final : public cell::Processor {
public:
    void prepare(const cell::ProcessSpec& spec) override { rate_ = spec.sampleRate; }
    void reset() noexcept override { note_ = -1; }
    void process(cell::AudioBlock<float> audio, const cell::ParameterBank& bank, const cell::Transport&,
                 std::span<const cell::MidiEvent> events) noexcept override {
        const auto level = static_cast<float>(bank.get(instrumentLevel));
        std::size_t next{};
        for (std::uint32_t frame = 0; frame < audio.frames; ++frame) {
            while (next < events.size() && events[next].frame == frame) apply(events[next++]);
            if (note_ < 0) continue;
            const auto sample = cell::dsp::analogOscillator(phase_, increment_, 0.f) * level * velocity_;
            phase_ += increment_;
            if (phase_ >= 1) phase_ -= 1;
            for (std::uint32_t channel = 0; channel < audio.channelCount; ++channel)
                audio.channel(channel)[frame] += sample;
        }
    }

private:
    void apply(cell::MidiEvent event) noexcept {
        const auto kind = event.data[0] & 0xf0;
        const auto channel = event.data[0] & 0x0f;
        if (kind == 0x90 && event.data[2]) {
            note_ = event.data[1];
            channel_ = channel;
            velocity_ = event.data[2] / 127.f;
            phase_ = 0.25;
            increment_ = std::min(440.0 * std::exp2((note_ - 69) / 12.0) / rate_, 0.49);
        } else if (channel == channel_ &&
                   (((kind == 0x80 || kind == 0x90) && event.data[1] == note_) ||
                    (kind == 0xb0 && (event.data[1] == 120 || event.data[1] == 123)))) {
            reset();
        }
    }

    int note_ = -1, channel_{};
    float velocity_{};
    double rate_ = instrumentRate, phase_{}, increment_{};
};
} // namespace

namespace cell::plugin {
const PluginDescriptor& descriptor() {
    static const PluginDescriptor result{.id = "cell.example.auv3-instrument-probe", .name = "Cell Instrument Probe",
        .vendor = "Cell", .version = "1", .channels = 2, .parameters = fields, .supportsMono = true, .acceptsMidi = true};
    return result;
}
std::unique_ptr<Processor> createProcessor() { return std::make_unique<InstrumentProbe>(); }
} // namespace cell::plugin
