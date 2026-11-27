// SPDX-License-Identifier: Apache-2.0
#pragma once
#include <cell/ui.hpp>
#include <span>
namespace cell::ui {
struct PaletteReplacement {
    Color from, to;
};
// GUI paint scope for migrating existing controls to semantic theme tokens.
// Storage belongs to the caller and outlives the scope. No global product
// state, allocations or file access; nested/offscreen paint scopes restore it.
class PaletteScope {
public:
    explicit PaletteScope(std::span<const PaletteReplacement> replacements) : previous_(active_) {
        active_ = replacements;
    }
    ~PaletteScope() {
        active_ = previous_;
    }
    PaletteScope(const PaletteScope&) = delete;
    PaletteScope& operator=(const PaletteScope&) = delete;
    static Color map(Color value) noexcept {
        if (active_.empty())
            return value;
        const auto rgb = value.argb & 0x00ffffffu;
        // Explicit semantic colors may be painted inside a legacy migration scope.
        // Recognize destinations first so their values aren't transformed twice.
        for (const auto& item : active_)
            if ((item.to.argb & 0x00ffffffu) == rgb)
                return value;
        for (const auto& item : active_)
            if ((item.from.argb & 0x00ffffffu) == rgb)
                return Color((value.argb & 0xff000000u) | (item.to.argb & 0x00ffffffu));
        return value;
    }

private:
    std::span<const PaletteReplacement> previous_;
    inline static thread_local std::span<const PaletteReplacement> active_{};
};
} // namespace cell::ui
