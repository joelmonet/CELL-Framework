// Original Cell Framework implementation; see LICENSE.
#include <cell/audio_device.hpp>
#include <algorithm>
#include <chrono>
#include <iostream>
#include <stdexcept>
#include <string_view>
#include <thread>

namespace {
constexpr unsigned channels = 2;

void silence(void*, float* output, const float*, unsigned frames) noexcept {
    std::fill_n(output, std::size_t(frames) * channels, 0.0f);
}
} // namespace

int main(int argc, char** argv) {
    using namespace cell::audio;
    try {
        const std::string_view mode = argc == 2 ? argv[1] : "auto";
        if (argc > 2 || (mode != "auto" && mode != "aaudio" && mode != "aaudio-low-latency" &&
                         mode != "aaudio-one-burst" && mode != "opensl"))
            throw std::invalid_argument("Usage: cell_android_audio_probe [auto|aaudio|aaudio-low-latency|aaudio-one-burst|opensl]");

        DeviceConfig config;
        if (mode.starts_with("aaudio"))
            config.backend = Backend::aAudio;
        else if (mode == "opensl")
            config.backend = Backend::openSL;
        config.inputChannels = 0;
        config.outputChannels = channels;
        config.sampleRate = 0;
        if (mode == "aaudio-low-latency")
            config.aAudioPlaybackMode = AAudioPlaybackMode::lowLatency;
        else if (mode == "aaudio-one-burst")
            config.aAudioPlaybackMode = AAudioPlaybackMode::oneBurst;

        Device device;
        const auto format = device.open(config, silence, nullptr);
        if (!device.start())
            throw std::runtime_error(device.error());
        const auto initial = device.aAudioPlaybackInfo();
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(3);
        while (std::chrono::steady_clock::now() < deadline) {
            const auto stats = device.stats();
            if (!stats.running || stats.interrupted || stats.frames >= format.sampleRate) break;
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
        }
        const auto playback = device.aAudioPlaybackInfo();
        const auto stats = device.stats();
        device.stop();
        if (!stats.running || stats.interrupted || stats.callbacks < 4 || stats.frames < format.sampleRate)
            throw std::runtime_error("Android output did not sustain playback for one second");
        std::cout << backendName(format.backend) << ": " << format.sampleRate << " Hz, "
                  << stats.callbacks << " callbacks, " << stats.frames << " frames, "
                  << stats.callbackOverruns << " callback overruns\n";
        if (playback) {
            std::cout << "AAudio endpoint " << playback->deviceId << ", "
                      << (playback->exclusive ? "exclusive" : "shared") << ", "
                      << (playback->lowLatency ? "low latency" : "standard latency") << ", "
                      << playback->framesPerBurst << " frames/burst, "
                      << playback->bufferFrames << '/' << playback->capacityFrames << " buffer frames ("
                      << 1000.0 * playback->bufferFrames / playback->sampleRate << " ms software buffer)";
            if (initial && initial->deviceId == playback->deviceId && initial->xruns >= 0 &&
                playback->xruns >= initial->xruns)
                std::cout << ", " << playback->xruns - initial->xruns << " stream xruns during sample";
            std::cout << '\n';
        }
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
