// Original Cell Framework implementation; see LICENSE.
#include "audio_effect.hpp"
#include <lv2/core/lv2.h>
#if defined(CELL_LV2_WEB_PROGRAMS)
#include "lv2_programs.hpp"
#endif
#if defined(CELL_LV2_METER_PORT)
#include <cell/web_meter.hpp>
#endif

namespace {

using cell::formats::AudioEffect;

struct Effect : AudioEffect {
    using AudioEffect::AudioEffect;
#if defined(CELL_LV2_METER_PORT)
    float* peak{};
    float* rms{};
#endif
#if defined(CELL_LV2_WEB_PROGRAMS)
    std::vector<double> observedPorts = std::vector<double>(controls.size(), std::numeric_limits<double>::quiet_NaN());
    template<class Operation>
    bool command(Operation operation, std::vector<cell::ParameterEdit>& edits) {
        const bool accepted = operation(engine);
        for (cell::ParameterEdit edit; engine.pollUiEdit(edit);) edits.push_back(edit);
        return accepted;
    }
#endif
};

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

void activate(LV2_Handle handle) noexcept {
    auto& effect = *static_cast<Effect*>(handle);
    cell::formats::activate(effect);
#if defined(CELL_LV2_WEB_PROGRAMS)
    effect.engine.parameters().copyValues(effect.observedPorts);
#endif
}

void run(LV2_Handle handle, std::uint32_t frames) noexcept {
    auto& effect = *static_cast<Effect*>(handle);
#if defined(CELL_LV2_WEB_PROGRAMS)
    cell::formats::run(effect, frames, effect.observedPorts);
#else
    cell::formats::run(effect, frames);
#endif
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

#if defined(CELL_LV2_WEB_PROGRAMS)
const void* extension(const char* uri) noexcept {
    if (!uri || std::string_view(uri) != cell::lv2::programAccessUri) return nullptr;
    using namespace cell::web::detail;
    static const cell::lv2::ProgramAccess access{
        [](LV2_Handle handle) {
            const auto& engine = static_cast<Effect*>(handle)->engine;
            return processorPrograms(engine, engine.parameters());
        },
        [](LV2_Handle handle, cell::ProgramAddress address, std::vector<cell::ParameterEdit>& edits) {
            return static_cast<Effect*>(handle)->command(
                [address](cell::Engine& engine) { return recallProcessorProgram(engine, address); }, edits);
        },
        [](LV2_Handle handle, std::string_view key, cell::web::ActionMode mode) {
            return processorSupportsAction(static_cast<Effect*>(handle)->engine, key, mode);
        },
        [](LV2_Handle handle, std::string_view key, cell::web::ActionPhase phase, std::vector<cell::ParameterEdit>& edits) {
            return static_cast<Effect*>(handle)->command(
                [=](cell::Engine& engine) { return processorAction(engine, key, phase); }, edits);
        }
    };
    return &access;
}
#else
constexpr auto extension = nullptr;
#endif
const LV2_Descriptor descriptor{CELL_LV2_URI, instantiate, connect, activate, run, nullptr, cleanup, extension};

} // namespace

LV2_SYMBOL_EXPORT const LV2_Descriptor* lv2_descriptor(std::uint32_t index) {
    return index == 0 ? &descriptor : nullptr;
}
