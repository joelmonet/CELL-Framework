// Original Cell Framework implementation; see LICENSE.
#include <cell/plugin.hpp>
#include <cell/synthesis.hpp>

namespace {
constexpr auto levelId = cell::stableId("level");
const std::array<cell::Parameter, 1> parameters{{{levelId, "level", "Level", "", 0, 1, .25}}};

class SineInstrument final : public cell::Processor {
public:
    void prepare(const cell::ProcessSpec& spec) override {
        rate_ = spec.sampleRate;
        attack_ = float(1.0 / (.002 * rate_));
        release_ = float(1.0 / (.012 * rate_));
        reset();
    }

    void reset() noexcept override {
        voices_ = {};
        nextVoice_ = 0;
    }

    void process(cell::AudioBlock<float> audio, const cell::ParameterBank& bank,
                 const cell::Transport&, std::span<const cell::MidiEvent> events) noexcept override {
        const auto gain = float(bank.get(levelId)) / voices_.size();
        std::size_t next{};
        for (unsigned frame = 0; frame < audio.frames; ++frame) {
            while (next < events.size() && events[next].frame == frame)
                receive(events[next++]);
            float sample{};
            for (auto& voice : voices_) {
                if (!voice.active) continue;
                if (voice.releasing) {
                    voice.envelope = std::max(0.f, voice.envelope - release_);
                    if (voice.envelope == 0) {
                        voice.active = false;
                        continue;
                    }
                } else {
                    voice.envelope = std::min(1.f, voice.envelope + attack_);
                }
                sample += cell::dsp::analogOscillator(voice.phase, voice.increment, 0.f)
                    * voice.envelope * voice.velocity;
                voice.phase += voice.increment;
                if (voice.phase >= 1.0) voice.phase -= 1.0;
            }
            for (unsigned channel = 0; channel < audio.channelCount; ++channel)
                audio.channel(channel)[frame] = gain * sample;
        }
    }

private:
    struct Voice {
        unsigned channel{}, key{};
        std::uint64_t age{};
        double phase{}, increment{};
        float velocity{}, envelope{};
        bool active{}, releasing{};
    };

    void noteOn(unsigned channel, unsigned key, unsigned velocity) noexcept {
        auto slot = std::ranges::find_if(voices_, [=](const Voice& voice) {
            return voice.active && voice.channel == channel && voice.key == key;
        });
        if (slot == voices_.end()) {
            // Allocate a free voice, then steal the oldest release or held note.
            slot = std::ranges::min_element(voices_, [](const Voice& a, const Voice& b) {
                const auto priority = [](const Voice& voice) {
                    return !voice.active ? 0 : voice.releasing ? 1 : 2;
                };
                return priority(a) == priority(b) ? a.age < b.age : priority(a) < priority(b);
            });
        }
        const auto frequency = 440.0 * std::exp2((double(key) - 69.0) / 12.0);
        *slot = {channel, key, nextVoice_++, .25, std::min(frequency / rate_, .49),
                 velocity / 127.f, 0.f, true, false};
    }

    void receive(const cell::MidiEvent& event) noexcept {
        if (event.size != 3 || event.isSysEx() || event.isUmp()) return;
        const auto status = event.data[0] & 0xf0;
        const auto channel = unsigned(event.data[0] & 0x0f);
        const auto key = event.data[1];
        const auto value = event.data[2];
        if (status == 0x90 && value) {
            noteOn(channel, key, value);
            return;
        }
        for (auto& voice : voices_) {
            if (!voice.active || voice.channel != channel) continue;
            if ((status == 0x80 || status == 0x90) && key == voice.key)
                voice.releasing = true;
            if (status == 0xb0 && key == 123) voice.releasing = true;
            if (status == 0xb0 && key == 120) voice.active = false;
        }
    }

    std::array<Voice, 8> voices_{};
    std::uint64_t nextVoice_{};
    double rate_{48000};
    float attack_{}, release_{};
};
}

namespace cell::plugin {
const PluginDescriptor& descriptor() {
    static const PluginDescriptor result{.id = "com.example.cell.sine", .name = "Cell Sine",
        .vendor = "Cell Project", .version = "0.1.0", .channels = 2,
        .parameters = parameters, .acceptsMidi = true};
    return result;
}
std::unique_ptr<Processor> createProcessor() { return std::make_unique<SineInstrument>(); }
}
