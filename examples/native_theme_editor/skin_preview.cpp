// SPDX-License-Identifier: Apache-2.0
#include <cell/files.hpp>
#include <cell/ui_skin_scene.hpp>

#include <cmath>
#include <iostream>

int main(int argc, char** argv) {
    try {
        if (argc != 3 && argc != 4)
            throw std::invalid_argument(
                "Usage: cell_skin_preview scene.cellskin.json output.png [device-scale]");
        const double scale = argc == 4 ? std::stod(argv[3]) : 1;
        if (!std::isfinite(scale) || scale < .25 || scale > 4)
            throw std::invalid_argument("Device scale must be between 0.25 and 4");
        const auto bytes = cell::files::readBounded(argv[1], cell::ui::skins::Scene::maxFileBytes);
        cell::ui::skins::Scene scene({reinterpret_cast<const char*>(bytes.data()), bytes.size()});
        const auto size = scene.size();
        const auto width = std::ceil(size.x * scale), height = std::ceil(size.y * scale);
        if (width > 8192 || height > 8192 || width * height > 16u * 1024u * 1024u)
            throw std::invalid_argument("Preview exceeds the native image size limit");
        cell::ui::Surface surface;
        surface.resize(int(width), int(height));
        surface.render([&](cell::ui::Canvas& canvas) {
            canvas.scale(scale, scale);
            scene.paint(canvas, scale);
        });
        if (!surface.writePng(argv[2]))
            throw std::runtime_error("Could not write the native preview PNG");
        const auto stats = scene.cacheStats();
        std::cout << width << 'x' << height << " native Canvas pixels, " << stats.layerBuilds
                  << " static material builds, " << stats.blurBuilds << " blur builds\n";
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
