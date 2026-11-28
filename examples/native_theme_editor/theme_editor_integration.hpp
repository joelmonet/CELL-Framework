// SPDX-License-Identifier: Apache-2.0
#pragma once
#include <cell/ui_theme_editor.hpp>
namespace cell::examples {
inline std::vector<ui::themes::Theme> exampleThemes() {
    ui::themes::Theme classic;
    classic.name = "CLASSIC";
    auto hdr = classic;
    hdr.name = "HDR";
    hdr.iconSet = "hdr";
    hdr.material = ui::themes::Material::sculpted;
    hdr.texture = .5;
    hdr.relief = .8;
    hdr.colors[unsigned(ui::themes::Role::panel)] = ui::Color(0xff14191c);
    auto modern = classic;
    modern.name = "MODERN";
    modern.iconSet = "modern";
    modern.material = ui::themes::Material::modern;
    modern.glow = .45;
    modern.radius = 9;
    return {classic, hdr, modern};
}
class ThemeExampleEditor final : public ui::Editor {
public:
    explicit ThemeExampleEditor(std::filesystem::path userDirectory)
        : library_(std::move(userDirectory), exampleThemes()), themeEditor_(library_) {}
    void openThemeEditor() {
        themeEditor_.open();
        if (invalidate)
            invalidate();
    } // Settings menu callback
    void paint(ui::Canvas& canvas, ui::Rect bounds) override {
        const auto state = library_.snapshot();
        const auto& theme = state.theme;
        panels_.theme(theme, state.revision);
        canvas.fill(bounds, theme.color(ui::themes::Role::background));
        panels_.paint(canvas, bounds.inset(12), nativeWindowScale);
        canvas.text({bounds.x + 30, bounds.y + 50}, theme.name, 22 * theme.fontScale,
                    theme.color(ui::themes::Role::ink));
        if (themeEditor_.isOpen()) {
            themeEditor_.paint(canvas, bounds);
        } else {
            canvas.text({bounds.x + 30, bounds.y + 80}, "Press T to edit the theme", 14,
                        theme.color(ui::themes::Role::muted));
        }
    }
    bool pointer(const ui::PointerEvent& e) override {
        return redrawIfHandled(themeEditor_.pointer(e, nativeWindowHandle));
    }
    bool key(int key) override {
        return keyboard({key});
    }
    bool keyboard(const ui::KeyEvent& e) override {
        if (!themeEditor_.isOpen() && !e.control && !e.command && !e.alt &&
            (e.key == 't' || e.key == 'T')) {
            openThemeEditor();
            return true;
        }
        return redrawIfHandled(themeEditor_.key(e, nativeWindowHandle));
    }
    bool accessibleAction(std::uint64_t id, ui::AccessibleAction action,
                          double value = 0) override {
        return redrawIfHandled(themeEditor_.accessible(id, action, value, nativeWindowHandle));
    }
    std::span<const ui::ControlInfo> controls() const noexcept override {
        return themeEditor_.isOpen() ? themeEditor_.controls() : std::span<const ui::ControlInfo>{};
    }
    std::uint64_t focusedControl() const noexcept override {
        return themeEditor_.focus();
    }
    bool tick() override {
        const bool dialogChanged = themeEditor_.tick();
        const auto revision = library_.revision();
        const bool changed = dialogChanged || revision != revision_;
        revision_ = revision;
        return changed;
    }
    const char* title() const noexcept override {
        return "Cell Theme Editor";
    }
    bool preserveAspectRatio() const noexcept override {
        return false;
    }
    ui::Point constrainSize(ui::Point size) const noexcept override {
        return {std::clamp(size.x, 480., 4000.), std::clamp(size.y, 400., 4000.)};
    }
    ui::Point preferredSize() const noexcept override {
        return {900, 760};
    }

private:
    bool redrawIfHandled(bool handled) {
        if (handled && invalidate)
            invalidate();
        return handled;
    }

    ui::themes::Library library_;
    ui::themes::PanelCache panels_;
    ui::themes::ThemeEditor themeEditor_;
    std::uint64_t revision_{};
};
} // namespace cell::examples
