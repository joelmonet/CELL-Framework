// SPDX-License-Identifier: Apache-2.0
#include <cell/files.hpp>
#include <cell/ui_skin_scene.hpp>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <iostream>

int main(int argc, char** argv) {
    try {
        if (argc < 3 || argc > 6)
            throw std::invalid_argument("Usage: cell_skin_preview scene.cellskin.json output.png "
                                        "[device-scale] [frames] [damage]");
        const double scale = argc >= 4 ? std::stod(argv[3]) : 1;
        const int frames = argc >= 5 ? std::stoi(argv[4]) : 1;
        const bool damageMode = argc == 6 && std::string_view(argv[5]) == "damage";
        if (argc == 6 && !damageMode)
            throw std::invalid_argument("Use damage to benchmark clipping to the moving control");
        if (frames < 1 || frames > 1000)
            throw std::invalid_argument("Frame count must be between 1 and 1000");
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
        auto damage = damageMode ? scene.controlBounds(1) : std::nullopt;
        if (damage)
            *damage = damage->inset(-8);
        const auto render = [&](std::optional<cell::ui::Rect> clip = {}) {
            const auto start = std::chrono::steady_clock::now();
            surface.render([&](cell::ui::Canvas& canvas) {
                canvas.scale(scale, scale);
                if (clip)
                    canvas.clip(*clip);
                scene.paint(canvas, scale);
            });
            return std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() -
                                                             start)
                .count();
        };
        if (frames > 1) {
            std::array<cell::ui::Point, 128> points;
            for (unsigned i = 0; i < points.size(); ++i) {
                const double x = double(i) / (points.size() - 1);
                points[i] = {x, .56 + .23 * std::sin(x * 5) - x * .24};
            }
            for (unsigned i = 0; i < 256; ++i)
                if (scene.setTrace(i, cell::ui::skins::TraceRole::frequencyResponse, points))
                    break;
        }
        const auto cold = render();
        const auto initial = scene.cacheStats();
        std::vector<double> warm;
        warm.reserve(frames - 1);
        for (int frame = 1; frame < frames; ++frame) {
            scene.setValue(1, double(frame % 101) / 100);
            warm.push_back(render(damage));
        }
        if (!warm.empty()) {
            std::sort(warm.begin(), warm.end());
            const auto stats = scene.cacheStats();
            std::cout << (damage ? "Clipped repaint. " : "Full repaint. ") << "Cold " << cold
                      << " ms, warm p50 " << warm[warm.size() / 2] << " ms, p95 "
                      << warm[std::size_t((warm.size() - 1) * .95)] << " ms, "
                      << stats.layerBuilds - initial.layerBuilds << " warm layer builds, "
                      << stats.blurBuilds - initial.blurBuilds << " warm blur builds, "
                      << stats.retainedBytes << " cached bytes\n";
        }
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
