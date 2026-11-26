// Original Cell Framework implementation; see LICENSE.
#include <cell/plugin.hpp>
#include <iostream>

namespace {
using cell::MidiEvent;
constexpr unsigned frames = 1024;
std::array<float, frames> render(std::span<const MidiEvent> events) {
    cell::Engine engine(cell::plugin::descriptor(), cell::plugin::createProcessor());
    engine.prepare(48000, frames, 2);
    cell::AudioBuffer<float> audio(2, frames);
    engine.process(audio.block(), {}, {}, events);
    const auto block = audio.block();
    if (!std::equal(block.channel(0), block.channel(0) + frames, block.channel(1)))
        throw std::runtime_error("Instrument channels produced different samples");
    std::array<float, frames> output{};
    std::copy_n(block.channel(0), frames, output.begin());
    return output;
}
}

int main() try {
    if (std::string_view(cell::plugin::descriptor().id) != CELL_EXPECTED_PLUGIN_ID)
        throw std::runtime_error("Instrument ID does not match plugin-id.txt");
    constexpr MidiEvent rootOn{9, {0x90, 69, 127}, 3}, rootOff{117, {0x80, 69, 0}, 3};
    constexpr MidiEvent fifthOn{31, {0x91, 76, 127}, 3}, stopChannel{200, {0xb1, 120, 0}, 3};
    const auto root = render(std::array{rootOn, rootOff});
    const auto fifth = render(std::array{fifthOn, stopChannel});
    const auto chord = render(std::array{rootOn, fifthOn, rootOff, stopChannel});
    if (chord[8] != 0 || chord[9] == 0 || fifth[30] != 0 || fifth[31] == 0 ||
        root[117] == 0 || fifth[200] != 0 ||
        std::any_of(chord.begin() + 694, chord.end(), [](float sample) { return sample != 0; }))
        throw std::runtime_error("MIDI edges, channel panic or release timing changed");
    for (unsigned frame = 0; frame < frames; ++frame)
        if (std::abs(chord[frame] - root[frame] - fifth[frame]) > 1e-7f)
            throw std::runtime_error("Overlapping notes did not sum independently");
    std::cout << "PASS sample-timed polyphony, release and channel panic\n";
} catch (const std::exception& error) {
    std::cerr << error.what() << '\n';
    return 1;
}
