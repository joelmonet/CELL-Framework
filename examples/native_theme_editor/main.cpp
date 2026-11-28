// SPDX-License-Identifier: Apache-2.0
#include "theme_editor_integration.hpp"
#include <chrono>
#include <iostream>
#include <thread>

int main() {
    try {
        const auto preferences =
            cell::File::getSpecialLocation(cell::File::userApplicationDataDirectory)
                .getChildFile("Cell Native Theme Editor");
        cell::examples::ThemeExampleEditor editor(preferences.nativePath());
        editor.openThemeEditor();
        cell::ui::Window window(editor);
        const auto size = editor.preferredSize();
        if (!window.open(0, int(size.x), int(size.y)))
            throw std::runtime_error("Native display is unavailable");
        window.setSizeLimits(480, 400, 4000, 4000);
        window.show(true);
        while (window.isOpen()) {
            window.poll();
            std::this_thread::sleep_for(std::chrono::milliseconds(16));
        }
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
