// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <cell/plugin.hpp>

namespace cell::auv3 {

inline constexpr std::array mainLayouts{
    AudioLayout::mono, AudioLayout::stereo, AudioLayout::surround51,
    AudioLayout::surround712, AudioLayout::surround714, AudioLayout::surround916,
};

inline bool supportsMainLayout(const PluginDescriptor& descriptor, AudioLayout layout, bool outputOnly = false) noexcept {
    if (descriptor.mainBusLayouts.empty()) {
        return (layout == AudioLayout::stereo && descriptor.channels == 2) ||
            (layout == AudioLayout::mono && (descriptor.channels == 1 ||
                (descriptor.channels == 2 && descriptor.supportsMono)));
    }
    return std::any_of(descriptor.mainBusLayouts.begin(), descriptor.mainBusLayouts.end(),
        [layout, outputOnly](const MainBusLayout& pair) {
            return pair.output == layout && (outputOnly || pair.input == layout);
        });
}

inline std::optional<AudioLayout> defaultMainLayout(const PluginDescriptor& descriptor, bool outputOnly = false) noexcept {
    std::optional<AudioLayout> fallback;
    for (auto layout : mainLayouts) {
        if (!supportsMainLayout(descriptor, layout, outputOnly)) continue;
        if (channelCount(layout) == descriptor.channels) return layout;
        if (!fallback) fallback = layout;
    }
    return fallback;
}

enum class Speaker {
    unknown, mono, left, right, center, lfe, rearLeft, rearRight, sideLeft, sideRight,
    topFrontLeft, topFrontRight, topRearLeft, topRearRight, topMiddleLeft, topMiddleRight, wideLeft, wideRight,
};

// These orders match the Cell VST3 and CLAP adapters.
inline constexpr std::array immersiveSpeakers{
    Speaker::left, Speaker::right, Speaker::center, Speaker::lfe,
    Speaker::rearLeft, Speaker::rearRight, Speaker::sideLeft, Speaker::sideRight,
    Speaker::topFrontLeft, Speaker::topFrontRight, Speaker::topRearLeft, Speaker::topRearRight,
    Speaker::topMiddleLeft, Speaker::topMiddleRight, Speaker::wideLeft, Speaker::wideRight,
};
inline constexpr std::array monoSpeakers{Speaker::mono};
inline constexpr std::array surround712Speakers{
    Speaker::left, Speaker::right, Speaker::center, Speaker::lfe,
    Speaker::rearLeft, Speaker::rearRight, Speaker::sideLeft, Speaker::sideRight,
    Speaker::topMiddleLeft, Speaker::topMiddleRight,
};

inline constexpr std::span<const Speaker> speakersFor(AudioLayout layout) noexcept {
    if (layout == AudioLayout::mono) return monoSpeakers;
    if (layout == AudioLayout::surround712) return surround712Speakers;
    return std::span(immersiveSpeakers).first(channelCount(layout));
}

inline constexpr auto maxChannels = channelCount(AudioLayout::surround916);
using ChannelOrder = std::array<unsigned, maxChannels>;
inline constexpr auto identityOrder = [] {
    ChannelOrder order{};
    for (unsigned channel = 0; channel < order.size(); ++channel) order[channel] = channel;
    return order;
}();

inline std::optional<ChannelOrder> channelOrder(AudioLayout layout, std::span<const Speaker> host) noexcept {
    const auto engine = speakersFor(layout);
    if (engine.size() != host.size()) return {};
    ChannelOrder order{};
    // Both sequences have the same length and the engine roles are unique.
    // Finding every role also rules out duplicate or unknown host roles.
    for (unsigned channel = 0; channel < engine.size(); ++channel) {
        const auto found = std::find(host.begin(), host.end(), engine[channel]);
        if (found == host.end()) return {};
        order[channel] = static_cast<unsigned>(found - host.begin());
    }
    return order;
}

} // namespace cell::auv3
