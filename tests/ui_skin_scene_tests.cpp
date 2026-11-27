// SPDX-License-Identifier: Apache-2.0
#include <cell/ui_skin_scene.hpp>
#include <cell/ui_sliced_artwork.hpp>
#include <cmath>

#include <cstring>
#include <iostream>
#include <stdexcept>

using namespace cell::ui;

namespace {
const std::string fixture = R"json({
    "schema":"cell.ui-skin-scene.v1",
    "width":180,"height":180,
    "colors":{"background":"#171615","text":"#ece1c8"},
    "assets":[],
    "materials":[
        {"fill":"#282624","end":"#171615","edge":"#645b4b","highlight":"#baa68a","edgeWidth":1,
         "radius":11,"opacity":1,"shape":"rounded","corners":[1,3,5,8],
         "gradient":{"kind":"conic","angle":35,"center":[0.5,0.5],"stops":[{"at":0,"color":"#005000","opacity":1},{"at":0.5,"color":"#201414","opacity":1},{"at":1,"color":"#005000","opacity":1}]},
         "borders":[{"width":1,"color":"#a17637"},{"width":1,"color":"#381820"}],
         "bevel":{"kind":"raised","angle":180,"width":1,"light":"#ffffff70","dark":"#00000080"},
         "innerShadow":{"x":0,"y":1,"sigma":2,"color":"#00000080"}},
        {"fill":"#4a4742","end":"#161514","edge":"#645b4b","highlight":"#baa68a","edgeWidth":1,
         "radius":8,"opacity":1,"shape":"ellipse","shadow":{"x":0,"y":3,"sigma":3,"color":"#00000060"}},
        {"fill":"#ece1c8","end":"#ece1c8","edge":"#00000000","highlight":"#00000000","edgeWidth":0,
         "radius":2,"opacity":1,"shape":"rounded"}
    ],
    "controls":[{"id":0,"kind":"knob","rect":[4,4,172,172],"value":0.2,"indicator":0.1,"label":"Drive",
        "layers":{"background":0,"face":1,"pointer":2,"indicator":2},"states":{"focus":{"background":2},"warning":{"face":0}},
        "geometry":{"angles":[-150,120],"pivot":[0.42,0.58],"arc":"bipolar","cap":"rotating","origin":0.6,"ticks":7},
        "stack":[{"material":2,"rect":[0.3,0.3,0.2,0.05],"anchor":[0,0],"pivot":[0,0.5],"transform":[0,0,0,1,1],
          "opacity":0.4,"visible":[0,1],"clip":true,"motion":{"source":"value","target":"rotate","range":[-150,120]}}]},
        {"id":1,"kind":"panel","rect":[10,130,160,44],"value":0.5,"label":"",
         "layers":{},"states":{},"analysis":{"background":"#121518","major":"#777777","minor":"#252525","labels":"#dddddd",
          "grid":[4,2,2],"axes":{"range":[-12,6],"precision":0,"unit":"dB"},
          "traces":{"waveform":{"color":"#42bcf2","width":2,"mode":"line"}}}}]
})json";

void require(bool test, const char* message) {
    if (!test)
        throw std::runtime_error(message);
}
std::uint32_t pixel(const Surface& surface, int x, int y) {
    const auto* row = static_cast<const std::byte*>(surface.pixels()) + y * surface.stride();
    return reinterpret_cast<const std::uint32_t*>(row)[x];
}

void testSlicedArtwork() {
    Surface image;
    const std::array<std::uint32_t, 9> colors{0xffff0000, 0xff994400, 0xff00ff00,
                                              0xff0000ff, 0xff777777, 0xff990099,
                                              0xffee9999, 0xff5555aa, 0xffffff00};
    image.setPixels(colors, 3, 3);
    ArtworkSlices slices{{1, 1, 1, 1},       {4, 5, 6, 7}, SliceMode::tile,
                         SliceMode::stretch, true,         {32, 16}};
    const auto minimum = artworkMinimumSize(slices);
    require(minimum.x == 32 && minimum.y == 16, "slice minimum mismatch");
    for (const double scale : {1., 1.25, 2.}) {
        const auto patches = artworkPatches({3, 3}, {2.3, 1.2, 48.4, 31.8}, slices, scale);
        for (const auto& patch : patches) {
            const auto device = patch.destination.x * scale;
            require(std::abs(std::round(device) - device) < 1.e-8,
                    "fractional slice edge was not pixel-aligned");
        }
        require(patches[0].destination.x + patches[0].destination.width == patches[1].destination.x,
                "neighboring slices disagree about a shared boundary");
        Surface target;
        target.resize(int(std::ceil(54 * scale)), int(std::ceil(36 * scale)));
        target.render([&](Canvas& canvas) {
            canvas.scale(scale, scale);
            canvas.fill({0, 0, 54, 36}, Color(0xff112233));
            drawSlicedArtwork(canvas, image, {2.3, 1.2, 48.4, 31.8}, slices, scale);
        });
        require(pixel(target, int(5 * scale), int(3 * scale)) == colors[0],
                "top left slice sampled the wrong artwork");
        require(pixel(target, int(49 * scale), int(3 * scale)) == colors[2],
                "top right slice sampled the wrong artwork");
        require(pixel(target, int(28 * scale), int(3 * scale)) == colors[1],
                "repeated slice did not use its own source region");
        require(pixel(target, int(20 * scale), int(15 * scale)) == 0xff112233,
                "transparent center covered the existing background");
    }
    slices.logical = {0, 6, 0, 6};
    slices.source = {0, 1, 0, 1};
    slices.transparentCenter = false;
    const auto three = artworkPatches({3, 3}, {0, 0, 32, 16}, slices, 1.);
    require(three[3].destination.width == 6 && three[4].destination.width == 20 &&
                three[5].destination.width == 6 && three[0].destination.height == 0,
            "three-slice did not preserve the two caps");
}

} // namespace

int main() {
    try {
        testSlicedArtwork();
        skins::Scene scene(fixture);
        require(scene.controlBounds(0).has_value(), "valid scene lost controls");
        require(!scene.setValue(99, .2) && !scene.setValue(0, 1.02),
                "bad dynamic updates were accepted");
        Surface surface;
        surface.resize(360, 360);
        const auto draw = [&] {
            surface.render([&](Canvas& canvas) {
                canvas.scale(2, 2);
                scene.paint(canvas, 2);
            });
        };
        draw();
        std::vector<std::byte> before(std::size_t(surface.stride()) * surface.height());
        std::memcpy(before.data(), surface.pixels(), before.size());
        const auto first = scene.cacheStats();
        require(first.layerBuilds >= 2 && first.blurBuilds >= 2,
                "native effects were not exercised");
        scene.setValue(0, .8);
        draw();
        const auto after = scene.cacheStats();
        require(after.layerBuilds == first.layerBuilds && after.blurBuilds == first.blurBuilds,
                "moving a knob rebuilt static materials");
        require(std::memcmp(before.data(), surface.pixels(), before.size()) != 0,
                "moving a knob had no effect on native pixels");
        require(scene.setIndicator(0, .7), "indicator update failed");
        draw();
        require(scene.cacheStats().layerBuilds == after.layerBuilds &&
                    scene.cacheStats().blurBuilds == after.blurBuilds,
                "updating an indicator rebuilt static materials");
        const auto beforeFocus = pixel(surface, 14, 150);
        require(scene.setState(0, skins::State::focus, true), "state update failed");
        draw();
        require(pixel(surface, 14, 150) != beforeFocus, "state did not resolve its material");
        require(scene.cacheStats().layerBuilds == after.layerBuilds,
                "simple focus material displaced an effect");
        require(scene.setState(0, skins::State::warning, true), "extended state update failed");
        draw();
        require(scene.cacheStats().retainedBytes <= 16u * 1024u * 1024u,
                "layer cache exceeded its budget");
        const std::array curve{Point{0, .3}, Point{.45, .83}, Point{1, .6}};
        require(scene.setTrace(1, skins::TraceRole::waveform, curve), "native trace update failed");
        require(!scene.setTrace(1, skins::TraceRole::sidechain, curve),
                "unconfigured trace was accepted");
        const auto traceStats = scene.cacheStats();
        draw();
        require(scene.cacheStats().layerBuilds == traceStats.layerBuilds,
                "analysis data rebuilt static layers");
        constexpr Point invalidPoint{-1, .4};
        require(!scene.setTrace(1, skins::TraceRole::waveform, {&invalidPoint, 1}),
                "trace accepted a point outside normalized coordinates");
        auto animated = fixture;
        const auto animationPosition = animated.find("\"label\":\"Drive\"");
        animated.insert(animationPosition,
                        "\"transition\":{\"duration\":900,\"easing\":\"ease-out\"},");
        skins::Scene animation(animated);
        require(!animation.needsFrame(), "static scene requested an animation frame");
        require(animation.setState(0, skins::State::focus, true) && animation.needsFrame(),
                "configured transition did not start");
        require(animation.setState(0, skins::State::focus, false) && animation.needsFrame(),
                "transition could not be interrupted");
        animation.setReducedMotion(true);
        require(!animation.needsFrame(), "reduced motion left an animation scheduled");
        require(animation.setState(0, skins::State::focus, true) && !animation.needsFrame(),
                "a new animation started with reduced motion enabled");
        auto corrupt = fixture;
        corrupt.replace(corrupt.find("\"pointer\":2"), 11, "\"pointer\":9");
        bool rejected = false;
        try {
            skins::Scene bad(corrupt);
        } catch (const std::exception&) {
            rejected = true;
        }
        require(rejected, "unknown native material reference was accepted");
        std::cout << "PASS native Canvas, real effects, state and cached dynamic paints\n";
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
