// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <cell/ui.hpp>
#include <optional>

namespace cell::ui::skins {

enum class State { enabled, hover, pressed, selected, focus, disabled };

struct CacheStats {
    std::size_t layers{}, retainedBytes{};
    std::uint64_t layerBuilds{}, blurBuilds{};
};

// Prepared, renderer-only description exported by the Designer. Instantiate on
// the GUI thread. No theme parsing or bitmap decoding runs in paint().
class Scene {
public:
    explicit Scene(std::string_view text);
    ~Scene();
    Scene(Scene&&) noexcept;
    Scene& operator=(Scene&&) noexcept;
    Scene(const Scene&) = delete;
    Scene& operator=(const Scene&) = delete;

    Point size() const noexcept;
    std::optional<Rect> controlBounds(unsigned id) const noexcept;
    bool setValue(unsigned id, double normalized) noexcept;
    bool setIndicator(unsigned id, double normalized) noexcept;
    bool setState(unsigned id, State state, bool active) noexcept;
    void paint(Canvas&, double deviceScale = 1);
    CacheStats cacheStats() const noexcept;

    static constexpr std::size_t maxFileBytes = 14u * 1024u * 1024u;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace cell::ui::skins
