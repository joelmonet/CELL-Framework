// Original Cell Framework implementation; see LICENSE.
#include "audio_effect.hpp"
#include <lv2/core/lv2.h>
#if defined(CELL_LV2_METER_PORT)
#include <cell/web_meter.hpp>
#endif

namespace {

using cell::formats::AudioEffect;

#if defined(CELL_LV2_METER_PORT)
struct Effect : AudioEffect {
    using AudioEffect::AudioEffect;
    float* peak{};
    float* rms{};
};
#else
using Effect = AudioEffect;
#endif

LV2_Handle instantiate(const LV2_Descriptor*, double rate, const char*,
                       const LV2_Feature* const*) noexcept {
    try {
        const auto& descriptor = cell::plugin::descriptor();
        if (!std::isfinite(rate) || rate <= 0 || !cell::formats::supportsAudioEffect(descriptor))
            return nullptr;
#if defined(CELL_LV2_METER_PORT)
        if (2u * descriptor.channels + descriptor.parameters.size() != CELL_LV2_METER_PORT) return nullptr;
#endif
        return new Effect(rate);
    } catch (...) {
        return nullptr;
    }
}

void connect(LV2_Handle handle, std::uint32_t port, void* data) noexcept {
    auto& effect = *static_cast<Effect*>(handle);
    auto* value = static_cast<float*>(data);
#if defined(CELL_LV2_METER_PORT)
    if (port == CELL_LV2_METER_PORT) { effect.peak = value; return; }
    if (port == CELL_LV2_METER_PORT + 1) { effect.rms = value; return; }
#endif
    cell::formats::connect(effect, port, value);
}

void activate(LV2_Handle handle) noexcept { cell::formats::activate(*static_cast<Effect*>(handle)); }

void run(LV2_Handle handle, std::uint32_t frames) noexcept {
    auto& effect = *static_cast<Effect*>(handle);
    cell::formats::run(effect, frames);
#if defined(CELL_LV2_METER_PORT)
    if (effect.peak || effect.rms) {
        const auto channels = static_cast<std::uint32_t>(effect.outputs.size());
        const auto levels = cell::web::OutputMeter::measure({effect.outputs.data(), channels, frames, 0});
        if (effect.peak) *effect.peak = std::min(levels.peak, 1.0f);
        if (effect.rms) *effect.rms = std::min(levels.rms, 1.0f);
    }
#endif
}

void cleanup(LV2_Handle handle) noexcept { delete static_cast<Effect*>(handle); }

const LV2_Descriptor descriptor{CELL_LV2_URI, instantiate, connect, activate, run, nullptr, cleanup, nullptr};

} // namespace

LV2_SYMBOL_EXPORT const LV2_Descriptor* lv2_descriptor(std::uint32_t index) {
    return index == 0 ? &descriptor : nullptr;
}
