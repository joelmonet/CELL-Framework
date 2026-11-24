// Original Cell Framework implementation; see LICENSE.
#include "gain.hpp"
#include <cell/audio_device.hpp>
#include <cell/text.hpp>
#include <algorithm>
#include <charconv>
#include <chrono>
#include <cmath>
#include <csignal>
#include <iostream>
#include <stdexcept>
#include <string_view>
#include <thread>

namespace {
constexpr unsigned channels = 2;
constexpr unsigned chunkFrames = 256;
constexpr double tau = 6.283185307179586;
constexpr std::string_view usage =
    "Usage: cell_gain_standalone [duplex|tone] [auto|alsa|pulse|jack] [gain-dB] [seconds]";
volatile std::sig_atomic_t stopped = 0;
void interrupt(int) { stopped = 1; }

cell::audio::Backend selectBackend(std::string_view name) {
    using cell::audio::Backend;
    if (name == "auto") return Backend::automatic;
    if (name == "alsa") return Backend::alsa;
    if (name == "pulse") return Backend::pulse;
    if (name == "jack") return Backend::jack;
    throw std::invalid_argument("Unknown audio backend");
}

struct Standalone {
    cell::Engine engine{cell::plugin::descriptor(), cell::plugin::createProcessor()};
    cell::AudioBuffer<float> buffer{channels, chunkFrames};
    cell::AudioBlock<float> preparedBlock = buffer.block();
    bool tone{};
    double phase{}, phaseStep{};

    static void callback(void* owner, float* output, const float* input, unsigned frames) noexcept {
        auto& app = *static_cast<Standalone*>(owner);
        for (unsigned start = 0; start < frames;) {
            const auto count = std::min(chunkFrames, frames - start);
            auto block = app.preparedBlock.slice(0, count);
            for (unsigned frame = 0; frame < count; ++frame) {
                float sample = 0.f;
                if (app.tone) {
                    sample = 0.05f * static_cast<float>(std::sin(app.phase));
                    app.phase += app.phaseStep;
                    if (app.phase >= tau) app.phase -= tau;
                }
                const auto offset = (std::size_t(start) + frame) * channels;
                for (unsigned channel = 0; channel < channels; ++channel)
                    block.channel(channel)[frame] = input ? input[offset + channel] : sample;
            }
            app.engine.process(block, {}, {});
            for (unsigned frame = 0; frame < count; ++frame) {
                const auto offset = (std::size_t(start) + frame) * channels;
                for (unsigned channel = 0; channel < channels; ++channel)
                    output[offset + channel] = block.channel(channel)[frame];
            }
            start += count;
        }
    }
};
}

int main(int argc, char** argv) {
    try {
        if (argc == 2 && std::string_view(argv[1]) == "--help") {
            std::cout << usage << '\n';
            return 0;
        }
        const std::string_view mode = argc > 1 ? argv[1] : "duplex";
        if (argc > 5 || (mode != "duplex" && mode != "tone"))
            throw std::invalid_argument("Expected duplex or tone");
        double gainDb = 0;
        if (argc > 3) {
            const std::string_view value = argv[3];
            const auto parsed = cell::detail::parseDoubleChars(value.data(), value.data() + value.size(), gainDb);
            if (parsed.ec != std::errc{} || parsed.ptr != value.data() + value.size() ||
                !std::isfinite(gainDb) || gainDb < -60 || gainDb > 12)
                throw std::invalid_argument("Gain must be in the range -60 to 12 dB");
        }
        unsigned seconds = 10;
        if (argc > 4) {
            const std::string_view value = argv[4];
            const auto parsed = std::from_chars(value.data(), value.data() + value.size(), seconds);
            if (parsed.ec != std::errc{} || parsed.ptr != value.data() + value.size() ||
                seconds == 0 || seconds > 86400)
                throw std::invalid_argument("Duration must be in the range 1 to 86400 seconds");
        }

        Standalone app;
        app.tone = mode == "tone";
        cell::audio::DeviceConfig config;
        config.backend = selectBackend(argc > 2 ? argv[2] : "auto");
        config.inputChannels = app.tone ? 0 : channels;
        config.outputChannels = channels;
        config.sampleRate = 0;

        cell::audio::Device device;
        const auto format = device.open(config, Standalone::callback, &app);
        if (format.inputChannels != config.inputChannels || format.outputChannels != channels ||
            !format.sampleRate)
            throw std::runtime_error("The endpoint could not provide the requested stereo format");
        app.phaseStep = tau * 440 / format.sampleRate;
        app.engine.parameters().set(cell::example::gainId, gainDb);
        app.engine.prepare(format.sampleRate, chunkFrames, channels);

        std::signal(SIGINT, interrupt);
        if (!device.start())
            throw std::runtime_error(device.error());
        const auto end = std::chrono::steady_clock::now() + std::chrono::seconds(seconds);
        while (!stopped && std::chrono::steady_clock::now() < end)
            std::this_thread::sleep_for(std::chrono::milliseconds(20));
        device.stop();
        const auto stats = device.stats();
        if (!stats.frames && !stopped)
            throw std::runtime_error("The audio endpoint did not deliver any frames");
        std::cout << cell::audio::backendName(format.backend) << ": " << format.sampleRate
                  << " Hz, " << stats.callbacks << " callbacks, " << stats.frames << " frames\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n' << usage << '\n';
        return 1;
    }
}
