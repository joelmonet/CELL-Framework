// Original Cell Framework implementation; see LICENSE.
#pragma once

#include <cell/audio.hpp>
#include <cell/json.hpp>
#include <atomic>
#include <cmath>

namespace cell::web {
// Single audio-thread producer, one or more readers. Each reading represents
// the most recent completed block across all output channels.
class OutputMeter {
public:
    struct Levels { float peak{}, rms{}; };

    void setEnabled(bool enabled) noexcept { enabled_.store(enabled, std::memory_order_relaxed); }

    void publish(AudioBlock<float> audio) noexcept {
        if (!enabled_.load(std::memory_order_relaxed) || !audio.frames || !audio.channelCount) return;
        const auto values = measure(audio);
        peak_.store(values.peak, std::memory_order_relaxed);
        rms_.store(values.rms, std::memory_order_relaxed);
    }

    // Disconnected output channels count as silence.
    static Levels measure(AudioBlock<float> audio) noexcept {
        if (!audio.frames || !audio.channelCount) return {};
        float peak = 0;
        double energy = 0;
        for (std::uint32_t channel = 0; channel < audio.channelCount; ++channel) {
            if (!audio.channels[channel]) continue;
            const auto* samples = audio.channel(channel);
            for (std::uint32_t frame = 0; frame < audio.frames; ++frame) {
                const auto sample = samples[frame];
                if (!std::isfinite(sample)) continue;
                peak = std::max(peak, std::abs(sample));
                energy += static_cast<double>(sample) * sample;
            }
        }
        const auto count = static_cast<double>(audio.channelCount) * audio.frames;
        return {peak, static_cast<float>(std::sqrt(energy / count))};
    }

    // Audio thread or stopped rendering.
    void clear() noexcept {
        peak_.store(0, std::memory_order_relaxed);
        rms_.store(0, std::memory_order_relaxed);
    }

    Levels levels() const noexcept {
        return {peak_.load(std::memory_order_relaxed), rms_.load(std::memory_order_relaxed)};
    }

    std::string snapshot() const { return snapshot(levels()); }

    // GUI thread. Quantize values to the same resolution as the meter display.
    static std::string snapshot(Levels values) {
        using V = json::Value;
        auto reading = [](const char* key, float amplitude) {
            const double level = std::isfinite(amplitude) ? amplitude : 0.0;
            const auto db = std::round(200 * std::log10(std::clamp(level, 0.001, 1.0))) / 10;
            return V(V::Object{{"key", V(std::string(key))}, {"minimum", V(-60.0)}, {"maximum", V(0.0)},
                {"unit", V(std::string("dBFS"))}, {"value", V(db)}});
        };
        return json::write(V(V::Object{{"type", V(std::string("meters"))}, {"meters", V(V::Array{
            reading("output-peak", values.peak), reading("output-rms", values.rms)})}}));
    }

private:
    static_assert(std::atomic<float>::is_always_lock_free && std::atomic<bool>::is_always_lock_free);
    std::atomic<bool> enabled_{false};
    std::atomic<float> peak_{0}, rms_{0};
};
} // namespace cell::web
