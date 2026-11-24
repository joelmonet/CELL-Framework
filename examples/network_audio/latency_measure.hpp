#pragma once

#include <cell/audio_file.hpp>
#include <cell/files.hpp>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <numeric>
#include <optional>
#include <span>
#include <stdexcept>
#include <vector>

namespace cell::network_audio::example {

inline constexpr int latencyProbeFrames = 256;

constexpr std::array<std::int16_t, latencyProbeFrames> latencyProbe() noexcept {
    std::array<std::int16_t, latencyProbeFrames> samples{};
    std::uint32_t state = 0x43454c4c; // Fixed seed for identical probes across platforms.
    for (int index = 0; index < latencyProbeFrames; ++index) {
        state = state * 1664525u + 1013904223u;
        const int noise = static_cast<int>(state >> 16) - 32768;
        const int envelope = std::min({index, latencyProbeFrames - 1 - index, 16});
        samples[index] = static_cast<std::int16_t>(noise * envelope / (8 * 16));
    }
    return samples;
}

inline void writeLatencyProbe(const File& destination, unsigned sampleRate, unsigned count) {
    const unsigned referenceFrame = sampleRate / 10;
    const unsigned totalFrames = sampleRate * 5 / 2;
    const auto header = audio::waveFileHeader({static_cast<double>(sampleRate), 2, 16, false},
                                              static_cast<std::int64_t>(totalFrames) * count);
    ByteWriter pcm;
    pcm.bytes.reserve(totalFrames * 4);
    pcm.bytes.resize(referenceFrame * 4);
    for (const auto sample : latencyProbe()) {
        const auto bits = static_cast<std::uint16_t>(sample);
        pcm.u32(std::uint32_t(bits) | (std::uint32_t(bits) << 16));
    }
    pcm.bytes.resize(totalFrames * 4);

    files::AtomicWriter output(destination.nativePath());
    output.write(header.bytes());
    for (unsigned index = 0; index < count; ++index) output.write(pcm.bytes);
    output.commit();
}

struct ProbeMatch {
    std::size_t offset;
    double correlation;
    bool inverted;
};

inline ProbeMatch findProbeMatch(const std::array<float, latencyProbeFrames>& reference,
                                 std::span<const float> search,
                                 const char* noMatchMessage, const char* ambiguousMessage) {
    if (search.size() < reference.size()) throw std::invalid_argument("The probe search window is too short");
    const auto finite = [](float sample) { return std::isfinite(sample); };
    if (!std::ranges::all_of(reference, finite) || !std::ranges::all_of(search, finite))
        throw std::invalid_argument("Capture contains a non-finite sample");

    const double mean = std::accumulate(reference.begin(), reference.end(), 0.0) / latencyProbeFrames;
    std::array<double, latencyProbeFrames> centered{};
    double referenceEnergy = 0.0;
    for (std::size_t index = 0; index < centered.size(); ++index) {
        centered[index] = reference[index] - mean;
        referenceEnergy += centered[index] * centered[index];
    }
    if (referenceEnergy == 0.0) throw std::invalid_argument("The reference window has no varying signal");

    std::vector<double> scores(search.size() - reference.size() + 1);
    std::size_t best = 0;
    for (std::size_t delay = 0; delay < scores.size(); ++delay) {
        double sum = 0.0, energy = 0.0, dot = 0.0;
        for (std::size_t index = 0; index < centered.size(); ++index) {
            const double sample = search[delay + index];
            sum += sample;
            energy += sample * sample;
            dot += centered[index] * sample;
        }
        // Mean removal makes the score independent of gain and DC offset.
        const double centeredEnergy = energy - sum * sum / latencyProbeFrames;
        if (centeredEnergy > referenceEnergy * 1e-6)
            scores[delay] = std::clamp(dot / std::sqrt(referenceEnergy * centeredEnergy), -1.0, 1.0);
        if (std::abs(scores[delay]) > std::abs(scores[best])) best = delay;
    }

    const auto strength = std::abs(scores[best]);
    if (strength < 0.70) throw std::runtime_error(noMatchMessage);
    for (std::size_t delay = 0; delay < scores.size(); ++delay) {
        const auto distance = delay > best ? delay - best : best - delay;
        if (distance > 2 && std::abs(scores[delay]) >= strength - 0.05)
            throw std::runtime_error(ambiguousMessage);
    }
    return {best, strength, scores[best] < 0.0};
}

struct LatencyMeasurement {
    std::int64_t referenceFrame;
    std::size_t delayFrames;
    double correlation;
    bool inverted;
};

inline std::shared_ptr<audio::AudioFileReader> openLatencyCapture(const File& source, unsigned sampleRate) {
    auto reader = audio::openWaveStream(source.createInputStream());
    if (!reader || reader->info().sampleRate != sampleRate || reader->info().channels != 2)
        throw std::invalid_argument("Latency measurement requires a stereo 48 kHz WAV");
    return reader;
}

inline LatencyMeasurement measureLatency(audio::AudioFileReader& reader,
                                         std::optional<std::int64_t> referenceFrame, int maxDelayFrames) {
    if (!referenceFrame) {
        const auto count = static_cast<int>(std::min(reader.info().frames, static_cast<std::int64_t>(10 * reader.info().sampleRate)));
        if (count < latencyProbeFrames)
            throw std::invalid_argument("Capture is too short to find the reference probe");
        std::vector<float> search(static_cast<std::size_t>(count));
        float* channels[]{search.data(), nullptr};
        if (!reader.read(channels, 2, 0, count))
            throw std::runtime_error("Could not read the reference channel");

        std::array<float, latencyProbeFrames> expected{};
        constexpr auto pcm = latencyProbe();
        std::ranges::transform(pcm, expected.begin(), [](std::int16_t sample) {
            return sample / 32768.0f;
        });
        const auto match = findProbeMatch(expected, search, "No strong match for the generated reference probe",
            "Ambiguous reference probe. Record one burst or supply its reference frame");
        referenceFrame = static_cast<std::int64_t>(match.offset);
    }

    const auto requiredFrames = maxDelayFrames + latencyProbeFrames;
    if (*referenceFrame > reader.info().frames || reader.info().frames - *referenceFrame < requiredFrames)
        throw std::invalid_argument("Capture is too short for the reference and delay window");

    std::array<float, latencyProbeFrames> reference{};
    std::vector<float> returned(static_cast<std::size_t>(requiredFrames));
    float* left[]{reference.data(), nullptr};
    float* right[]{nullptr, returned.data()};
    if (!reader.read(left, 2, *referenceFrame, latencyProbeFrames) ||
        !reader.read(right, 2, *referenceFrame, requiredFrames))
        throw std::runtime_error("Could not read the latency measurement window");

    const auto match = findProbeMatch(reference, returned, "No strong match in the return channel",
        "Ambiguous return delay. Use an isolated probe or a shorter search window");
    return {*referenceFrame, match.offset, match.correlation, match.inverted};
}

} // namespace cell::network_audio::example
