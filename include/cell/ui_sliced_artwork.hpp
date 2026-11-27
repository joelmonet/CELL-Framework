// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <cell/ui.hpp>

namespace cell::ui {

enum class SliceMode { stretch, tile };

// Insets are top, right, bottom, left in source pixels and destination logical
// units. Zero insets on two opposite sides produce a three-slice.
struct ArtworkSlices {
    std::array<double, 4> source{}, logical{};
    SliceMode edges = SliceMode::stretch, center = SliceMode::stretch;
    bool transparentCenter = false;
    Point minimum;
};

struct ArtworkPatch {
    Rect source, destination;
    bool repeatX{}, repeatY{};
};

Point artworkMinimumSize(const ArtworkSlices&) noexcept;
std::array<ArtworkPatch, 9> artworkPatches(Point imageSize, Rect destination, const ArtworkSlices&,
                                           double deviceScale = 1);
void drawSlicedArtwork(Canvas&, const Surface&, Rect destination, const ArtworkSlices&,
                       double deviceScale = 1);

} // namespace cell::ui
