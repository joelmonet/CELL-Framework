// Original Cell Framework implementation; see LICENSE.
#pragma once
#include <cell/plugin.hpp>

namespace cell::formats {

inline bool supportsAudioEffect(const PluginDescriptor& descriptor) noexcept {
    return !descriptor.supportsSidechain && !descriptor.acceptsMidi && !descriptor.producesMidi;
}

inline constexpr unsigned blockSize = 512;

struct AudioEffect {
    cell::Engine engine;
    cell::AudioBuffer<float> scratch;
    std::vector<const float*> inputs;
    std::vector<float*> outputs;
    std::vector<const float*> controls;
    std::vector<cell::ParameterEvent> events;

    explicit AudioEffect(double rate)
        : engine(cell::plugin::descriptor(), cell::plugin::createProcessor()) {
        const auto& descriptor = engine.descriptor();
        const auto channels = descriptor.channels;
        engine.prepare(rate, blockSize, channels);
        scratch.resize(channels, blockSize);
        inputs.resize(channels);
        outputs.resize(channels);
        controls.resize(descriptor.parameters.size());
        events.reserve(controls.size());
    }
};

inline void connect(AudioEffect& instance, std::size_t port, float* data) noexcept {
    if (port < instance.inputs.size()) {
        instance.inputs[port] = static_cast<const float*>(data);
        return;
    }
    port -= static_cast<std::uint32_t>(instance.inputs.size());
    if (port < instance.outputs.size()) {
        instance.outputs[port] = static_cast<float*>(data);
        return;
    }
    port -= static_cast<std::uint32_t>(instance.outputs.size());
    if (port < instance.controls.size())
        instance.controls[port] = static_cast<const float*>(data);
}

inline void activate(AudioEffect& instance) noexcept {
    const auto parameters = instance.engine.descriptor().parameters;
    for (std::size_t i = 0; i < parameters.size(); ++i) {
        const auto& parameter = parameters[i];
        instance.engine.parameters().set(parameter.id,
            instance.controls[i] ? *instance.controls[i] : parameter.defaultValue);
    }
    instance.engine.reset();
}

inline void run(AudioEffect& instance, std::size_t frames, std::span<double> observedPorts = {}) noexcept {
    const auto parameters = instance.engine.descriptor().parameters;
    instance.events.clear();
    for (std::size_t i = 0; i < parameters.size(); ++i) {
        const auto& parameter = parameters[i];
        const auto value = parameter.clamp(instance.controls[i] ? *instance.controls[i] : parameter.defaultValue);
        // A successful editor recall can precede the host's control-port echo.
        // An unchanged port value must not replace the newly recalled value.
        if (!observedPorts.empty()) {
            if (value == observedPorts[i]) continue;
            observedPorts[i] = value;
        }
        if (value != instance.engine.parameters().get(parameter.id))
            instance.events.push_back({0, parameter.id, value});
    }
    for (std::size_t offset = 0; offset < frames;) {
        const auto count = static_cast<unsigned>(std::min<std::size_t>(blockSize, frames - offset));
        const auto block = instance.scratch.block().slice(0, count);
        for (unsigned c = 0; c < block.channelCount; ++c) {
            if (instance.inputs[c])
                std::copy_n(instance.inputs[c] + offset, count, block.channel(c));
            else
                std::fill_n(block.channel(c), count, 0.0f);
        }
        instance.engine.process(block, instance.events);
        instance.events.clear();
        for (unsigned c = 0; c < block.channelCount; ++c) {
            if (instance.outputs[c])
                std::copy_n(block.channel(c), count, instance.outputs[c] + offset);
        }
        offset += count;
    }
}

} // namespace cell::formats
