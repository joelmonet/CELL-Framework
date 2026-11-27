// SPDX-License-Identifier: Apache-2.0
#include <cell/ui_skin_scene.hpp>

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
         "radius":11,"opacity":1,"shape":"rounded","innerShadow":{"x":0,"y":1,"sigma":2,"color":"#00000080"}},
        {"fill":"#4a4742","end":"#161514","edge":"#645b4b","highlight":"#baa68a","edgeWidth":1,
         "radius":8,"opacity":1,"shape":"ellipse","shadow":{"x":0,"y":3,"sigma":3,"color":"#00000060"}},
        {"fill":"#ece1c8","end":"#ece1c8","edge":"#00000000","highlight":"#00000000","edgeWidth":0,
         "radius":2,"opacity":1,"shape":"rounded"}
    ],
    "controls":[{"id":0,"kind":"knob","rect":[4,4,172,172],"value":0.2,"indicator":0.1,"label":"Drive",
        "layers":{"background":0,"face":1,"pointer":2,"indicator":2},"states":{"focus":{"background":2}}}]
})json";

void require(bool test, const char* message) {
    if (!test)
        throw std::runtime_error(message);
}
} // namespace

int main() {
    try {
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
        require(first.layerBuilds >= 3 && first.blurBuilds >= 2,
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
        require(scene.setState(0, skins::State::focus, true), "state update failed");
        draw();
        require(scene.cacheStats().layerBuilds > after.layerBuilds,
                "state did not resolve its material");
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
