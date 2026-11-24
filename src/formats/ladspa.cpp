// Original Cell Framework implementation; see LICENSE.
#include "audio_effect.hpp"
#include <ladspa.h>
#if CELL_HAS_DSSI
#include <dssi.h>
#endif

namespace {
using cell::formats::AudioEffect;

LADSPA_Handle instantiate(const LADSPA_Descriptor*, unsigned long rate) noexcept {
    try {
        return rate ? new AudioEffect(static_cast<double>(rate)) : nullptr;
    } catch (...) {
        return nullptr;
    }
}
void connect(LADSPA_Handle handle, unsigned long port, LADSPA_Data* value) noexcept {
    cell::formats::connect(*static_cast<AudioEffect*>(handle), port, value);
}
void activate(LADSPA_Handle handle) noexcept { cell::formats::activate(*static_cast<AudioEffect*>(handle)); }
void run(LADSPA_Handle handle, unsigned long frames) noexcept {
    cell::formats::run(*static_cast<AudioEffect*>(handle), frames);
}
void deactivate(LADSPA_Handle) noexcept {}
void cleanup(LADSPA_Handle handle) noexcept { delete static_cast<AudioEffect*>(handle); }

struct Metadata {
    LADSPA_Descriptor descriptor{};
    std::vector<LADSPA_PortDescriptor> ports;
    std::vector<LADSPA_PortRangeHint> hints;
    std::vector<std::string> labels;
    std::vector<const char*> names;

    explicit Metadata(const cell::PluginDescriptor& source) {
        for (const auto direction : {LADSPA_PORT_INPUT, LADSPA_PORT_OUTPUT}) {
            for (unsigned channel = 0; channel < source.channels; ++channel) {
                ports.push_back(direction | LADSPA_PORT_AUDIO);
                hints.push_back({});
                labels.push_back((direction == LADSPA_PORT_INPUT ? "Input " : "Output ") +
                                 std::to_string(channel + 1));
            }
        }
        for (const auto& parameter : source.parameters) {
            int hint = LADSPA_HINT_BOUNDED_BELOW | LADSPA_HINT_BOUNDED_ABOVE;
            if (parameter.defaultValue == 0) hint |= LADSPA_HINT_DEFAULT_0;
            else if (parameter.defaultValue == 1) hint |= LADSPA_HINT_DEFAULT_1;
            else if (parameter.defaultValue == parameter.minimum) hint |= LADSPA_HINT_DEFAULT_MINIMUM;
            else if (parameter.defaultValue == parameter.maximum) hint |= LADSPA_HINT_DEFAULT_MAXIMUM;
            ports.push_back(LADSPA_PORT_INPUT | LADSPA_PORT_CONTROL);
            hints.push_back({hint, static_cast<float>(parameter.minimum), static_cast<float>(parameter.maximum)});
            labels.push_back(parameter.name);
        }
        for (const auto& label : labels)
            names.push_back(label.c_str());

        descriptor.UniqueID = CELL_LADSPA_ID;
        descriptor.Label = CELL_LADSPA_LABEL;
        descriptor.Name = source.name;
        descriptor.Maker = source.vendor;
        descriptor.Copyright = "See Cell Framework LICENSE";
        descriptor.PortCount = ports.size();
        descriptor.PortDescriptors = ports.data();
        descriptor.PortRangeHints = hints.data();
        descriptor.PortNames = names.data();
        descriptor.instantiate = instantiate;
        descriptor.connect_port = connect;
        descriptor.activate = activate;
        descriptor.run = run;
        descriptor.deactivate = deactivate;
        descriptor.cleanup = cleanup;
    }
};
} // namespace

extern "C" __attribute__((visibility("default"))) const LADSPA_Descriptor* ladspa_descriptor(unsigned long index) {
    if (index)
        return nullptr;
    try {
        const auto& source = cell::plugin::descriptor();
        if (!cell::formats::supportsAudioEffect(source))
            return nullptr;
        static const Metadata metadata(source);
        return &metadata.descriptor;
    } catch (...) {
        return nullptr;
    }
}

#if CELL_HAS_DSSI
extern "C" __attribute__((visibility("default"))) const DSSI_Descriptor* dssi_descriptor(unsigned long index) {
    if (index)
        return nullptr;
    const auto* effect = ladspa_descriptor(0);
    if (!effect)
        return nullptr;
    static const DSSI_Descriptor descriptor = [effect] {
        DSSI_Descriptor value{};
        value.DSSI_API_Version = 1;
        value.LADSPA_Plugin = effect;
        return value;
    }();
    return &descriptor;
}
#endif
