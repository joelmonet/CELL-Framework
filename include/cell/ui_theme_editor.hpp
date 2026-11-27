// SPDX-License-Identifier: Apache-2.0
#pragma once
#include <cctype>
#include <cell/file_dialog.hpp>
#include <cell/ui_palette.hpp>
#include <cell/ui_theme_library.hpp>
#include <cstdio>
#include <optional>
namespace cell::ui::themes {
class ThemeEditor {
public:
    explicit ThemeEditor(Library& library) : library_(library) {}
    void open() {
        if (open_)
            return;
        before_ = library_.snapshot();
        stockIndex_ = std::max(0, before_.stockIndex);
        open_ = true;
        focusField_ = 0;
        scroll_ = 0;
        status_ = "Changes preview live. Stock themes are protected.";
        entry_.reset();
    }
    bool isOpen() const noexcept {
        return open_;
    }
    void close(bool apply) {
        if (!open_)
            return;
        try {
            if (apply)
                library_.apply();
            else
                library_.preview(before_);
            open_ = false;
            focusField_ = 0;
            drag_.reset();
            entry_.reset();
            controls_.clear();
            focusTargets_.clear();
            dialog_.cancel();
        } catch (const std::exception& e) {
            status_ = e.what();
        }
    }
    ~ThemeEditor() {
        if (open_)
            try {
                library_.preview(before_);
            } catch (...) {
            }
    }
    bool tick() {
        if (auto result = dialog_.poll()) {
            if (!result->cancelled)
                try {
                    if (!result->error.empty())
                        throw std::runtime_error(result->error);
                    if (dialogField_ == 12) {
                        library_.exportFile(result->path);
                        status_ = "THEME EXPORTED";
                    } else if (dialogField_ == 10) {
                        library_.loadUser(result->path);
                        status_ = "USER THEME / PREVIEW";
                    } else {
                        library_.importFile(result->path);
                        status_ = "THEME IMPORTED / PREVIEW";
                    }
                } catch (const std::exception& e) {
                    status_ = e.what();
                }
            return true;
        }
        return false;
    }
    std::span<const ui::ControlInfo> controls() const noexcept {
        return controls_;
    }
    std::uint64_t focus() const noexcept {
        return focusField_ ? identity(focusField_) : 0;
    }
    void paint(ui::Canvas& c, ui::Rect bounds) {
        using namespace ui;
        PaletteScope exactColors(std::span<const PaletteReplacement>{});
        controls_.clear();
        focusTargets_.clear();
        if (!open_)
            return;
        c.fill(bounds, Color(0xa0000000));
        if (bounds.width < 200 || bounds.height < 200)
            return;
        const auto state = library_.snapshot();
        const auto& theme = state.theme;
        const auto text = theme.color(Role::ink), muted = theme.color(Role::muted);
        const double width = std::min(780., bounds.width - 24),
                     height = std::min(690., bounds.height - 24);
        box_ = {bounds.x + (bounds.width - width) / 2, bounds.y + (bounds.height - height) / 2,
                width, height};
        themes::panel(c, box_, theme);
        label(c, {box_.x + 16, box_.y + 10, width - 80, 30}, "THEME EDITOR", 20, text, true);
        button(c, theme, {box_.x + width - 48, box_.y + 11, 30, 28}, 9, "X");
        label(c, {box_.x + 16, box_.y + 43, width - 32, 19},
              state.stockIndex >= 0 ? "STOCK THEME / SAVING CREATES A USER COPY"
                                    : "USER THEME / LIVE PREVIEW",
              10, muted);
        const double x = box_.x + 16, w = width - 32;
        maxScroll_ =
            std::max(0., 476. + 46 * std::ceil(library_.stocks().size() / 3.) - (height - 170));
        scroll_ = std::clamp(scroll_, 0., maxScroll_);
        clip_ = {x, box_.y + 70, w, height - 170};
        clipping_ = true;
        c.save();
        c.clip(clip_);
        double y = box_.y + 70 - scroll_;
        button(c, theme, {x, y, w, 31}, 20,
               entry_ && entry_->field == 20 ? entry_->text + "_" : theme.name,
               ControlInfo::Role::textbox);
        y += 40;
        for (unsigned i = 0; i < library_.stocks().size(); ++i)
            button(c, theme, {x + (i % 3) * (w + 8) / 3, y + (i / 3) * 46, (w - 16) / 3, 36},
                   1000 + i, library_.stock(i).name, ControlInfo::Role::choice,
                   state.stockIndex == int(i));
        y += 46 * std::ceil(library_.stocks().size() / 3.);
        const char* actions[]{"LOAD USER THEME", "IMPORT…", "EXPORT…", "SAVE USER THEME"};
        for (unsigned i = 0; i < 4; ++i)
            button(c, theme, {x + i * (w + 6) / 4, y, (w - 18) / 4, 29}, 10 + i, actions[i]);
        y += 42;
        label(c, {x, y, w, 20}, "COLORS / CLICK A VALUE TO ENTER HEX", 11, muted);
        y += 26;
        const double col = (w + 10) / 3;
        for (unsigned i = 0; i < roleNames.size(); ++i) {
            Rect r{x + (i % 3) * col, y + (i / 3) * 43, col - 10, 37};
            const auto color = theme.colors[i];
            c.rounded({r.x, r.y + 4, 24, 26}, 3, color);
            c.strokeRounded({r.x, r.y + 4, 24, 26}, 3, theme.color(Role::edge));
            std::string caption(themes::roleNames[i]);
            for (auto& ch : caption)
                ch = char(std::toupper(static_cast<unsigned char>(ch)));
            label(c, {r.x + 31, r.y, r.width - 31, 12}, caption, 8, muted);
            button(c, theme, {r.x + 30, r.y + 14, r.width - 30, 21}, 100 + i,
                   entry_ && entry_->field == 100 + i ? entry_->text + "_" : Theme::hex(color),
                   ControlInfo::Role::textbox);
        }
        y += 139;
        label(c, {x, y, w, 20}, "MATERIAL / LIGHTING", 11, muted);
        y += 25;
        constexpr std::array materialLabels{"CLASSIC", "SCULPTED", "MODERN"};
        constexpr std::array iconLabels{"CLASSIC ICONS", "HDR ICONS", "MODERN ICONS"};
        for (unsigned i = 0; i < materialNames.size(); ++i)
            button(c, theme, {x + i * (w + 8) / 3, y, (w - 16) / 3, 26}, 310 + i, materialLabels[i],
                   ControlInfo::Role::choice, theme.material == Material(i));
        y += 32;
        for (unsigned i = 0; i < iconSetNames.size(); ++i)
            button(c, theme, {x + i * (w + 8) / 3, y, (w - 16) / 3, 26}, 300 + i, iconLabels[i],
                   ControlInfo::Role::choice, theme.iconSet == iconSetNames[i]);
        y += 36;
        for (unsigned i = 0; i < sliders.size(); ++i) {
            const auto& slider = sliders[i];
            const auto minimum = slider.minimum, maximum = slider.maximum;
            const double cw = (w - 16) / 2;
            Rect r{x + (i % 2) * (cw + 16), y + (i / 2) * 45, cw, 39};
            const double value = theme.*slider.member;
            label(c, {r.x, r.y, r.width * .7, 16}, slider.label, 9, muted);
            char numeric[24];
            std::snprintf(numeric, sizeof numeric,
                          i == 3   ? "%.0f"
                          : i == 4 ? "%.2fx"
                                   : "%.0f%%",
                          i < 3 ? value * 100 : value);
            label(c, {r.x + r.width * .7, r.y, r.width * .3, 16}, numeric, 10, text);
            const Rect track{r.x + 5, r.y + 24, r.width - 10, 5};
            c.rounded(track, 2, theme.color(Role::edge));
            const double n = (value - minimum) / (maximum - minimum);
            c.rounded({track.x, track.y, track.width * n, track.height}, 2,
                      theme.color(Role::accent));
            c.circle({track.x + track.width * n, track.y + 2.5}, 6, text);
            control({r.x, r.y + 17, r.width, 21}, 200 + i, slider.label, ControlInfo::Role::slider,
                    value, minimum, maximum);
            if (focusField_ == 200 + i)
                c.strokeRounded({r.x, r.y + 17, r.width, 21}, 3, theme.color(Role::accent));
        }
        c.restore();
        clipping_ = false;
        const double bottom = box_.y + height - 86;
        label(c, {x, bottom, w, 27}, status_, 10, muted);
        button(c, theme, {x, bottom + 35, (w - 16) / 3, 32}, 14, "RESET SELECTED STOCK");
        button(c, theme, {x + (w + 8) / 3, bottom + 35, (w - 16) / 3, 32}, 15, "CANCEL");
        button(c, theme, {x + 2 * (w + 8) / 3, bottom + 35, (w - 16) / 3, 32}, 16, "APPLY");
    }
    bool pointer(const ui::PointerEvent& event, std::uintptr_t parent) {
        if (!open_)
            return false;
        using A = ui::PointerAction;
        if (event.action == A::cancel) {
            drag_.reset();
            return true;
        }
        if (drag_) {
            if (event.identity != drag_->identity)
                return true;
            if (event.action == A::move)
                slide(drag_->field, drag_->bounds, event.position.x);
            if (event.action == A::up)
                drag_.reset();
            return true;
        }
        if (event.action == A::wheel) {
            if (std::isfinite(event.wheel))
                scroll_ = std::clamp(scroll_ - event.wheel * 32, 0., maxScroll_);
            return true;
        }
        if (event.action != A::down || event.button != ui::PointerButton::primary)
            return true;
        for (const auto& info : controls_) {
            if (!info.bounds.contains(event.position))
                continue;
            const auto field = unsigned(info.identity - base);
            focusField_ = field;
            if (isSlider(field)) {
                commitEntry();
                if (!entry_) {
                    drag_ = Drag{field, info.bounds, event.identity};
                    slide(field, info.bounds, event.position.x);
                }
            } else {
                activate(field, parent);
            }
            return true;
        }
        commitEntry();
        return true;
    }
    bool key(const ui::KeyEvent& event, std::uintptr_t parent = 0) {
        if (!open_)
            return false;
        const int key = event.key;
        if (key == 27) {
            if (entry_)
                entry_.reset();
            else
                close(false);
            return true;
        }
        if (event.control || event.command || event.alt) {
            if (entry_ && (event.control || event.command) && (key == 'a' || key == 'A')) {
                entry_->selectAll = true;
                return true;
            }
            return false;
        }
        if (key == 9 || key == ui::keys::reverseTab) {
            commitEntry();
            if (!entry_)
                moveFocus(event.shift || key == ui::keys::reverseTab);
            return true;
        }
        if (!entry_) {
            if (key == 13 && focusField_ && !isSlider(focusField_)) {
                activate(focusField_, parent);
                return true;
            }
            if (isSlider(focusField_)) {
                const auto& slider = sliders[focusField_ - 200];
                if (key == ui::keys::home || key == ui::keys::end) {
                    changeSlider(focusField_,
                                 key == ui::keys::home ? slider.minimum : slider.maximum);
                    return true;
                }
                if (key == ui::keys::up || key == ui::keys::down || key == ui::keys::left ||
                    key == ui::keys::right) {
                    const double direction = key == ui::keys::up || key == ui::keys::right ? 1 : -1;
                    changeSlider(focusField_, {}, direction * slider.step);
                    return true;
                }
            }
            return false; // Let the host handle Space and other unclaimed shortcuts.
        }
        if (key == 13) {
            commitEntry();
            return true;
        }
        if (key == 8 || key == ui::keys::deleteKey) {
            if (entry_->selectAll) {
                entry_->text.clear();
            } else if (!entry_->text.empty()) {
                auto last = entry_->text.size() - 1;
                while (last && (static_cast<unsigned char>(entry_->text[last]) & 0xc0) == 0x80)
                    --last;
                entry_->text.resize(last);
            }
            entry_->selectAll = false;
            return true;
        }
        const auto ch = event.text ? event.text : (key >= 32 && key < 127 ? char32_t(key) : 0);
        if (ch >= 32 && ch != 127) {
            const auto text = Text::charToString(ch);
            const auto used = entry_->selectAll ? 0 : entry_->text.size();
            const auto limit = entry_->field == 20 ? Theme::maxNameBytes : 9;
            if (used + text.getNumBytesAsUTF8() <= limit) {
                if (entry_->selectAll)
                    entry_->text.clear();
                entry_->selectAll = false;
                entry_->text += text.utf8();
            }
        }
        return true;
    }
    bool accessible(std::uint64_t id, ui::AccessibleAction action, double value,
                    std::uintptr_t parent) {
        if (!open_ || id < base ||
            std::none_of(
                controls_.begin(), controls_.end(),
                [id](const auto& control) { return control.identity == id; }))
            return false;
        const auto field = unsigned(id - base);
        if (action == ui::AccessibleAction::focus) {
            focusField_ = field;
            return true;
        }
        if (isSlider(field)) {
            const auto& slider = sliders[field - 200];
            if (action == ui::AccessibleAction::setValue && std::isfinite(value))
                return changeSlider(field, value);
            if (action == ui::AccessibleAction::increment ||
                action == ui::AccessibleAction::decrement)
                return changeSlider(field, {},
                                    action == ui::AccessibleAction::increment ? slider.step
                                                                              : -slider.step);
            return false;
        }
        if (action == ui::AccessibleAction::activate) {
            focusField_ = field;
            activate(field, parent);
            return true;
        }
        return false;
    }

private:
    static constexpr std::uint64_t base = 0x5448454d45000000ull;
    Library& library_;
    bool open_{}, clipping_{};
    unsigned focusField_{}, dialogField_{};
    int stockIndex_{};
    double scroll_{}, maxScroll_{};
    ui::Rect clip_;
    Snapshot before_;
    ui::Rect box_;
    std::string status_;
    ui::FileDialog dialog_;
    std::vector<ui::ControlInfo> controls_;
    struct FocusTarget {
        unsigned field;
        ui::Rect bounds;
        bool scrolls;
    };
    std::vector<FocusTarget> focusTargets_;
    struct Entry {
        unsigned field{};
        std::string text;
        bool selectAll = true;
    };
    std::optional<Entry> entry_;
    struct Drag {
        unsigned field{};
        ui::Rect bounds;
        ui::PointerIdentity identity;
    };
    std::optional<Drag> drag_;
    static std::uint64_t identity(unsigned field) {
        return base + field;
    }
    struct Slider {
        const char* label;
        double Theme::* member;
        double minimum, maximum, step;
    };
    static constexpr std::array<Slider, 5> sliders{{
        {"GLOW", &Theme::glow, 0, 1, .025},
        {"SURFACE TEXTURE", &Theme::texture, 0, 1, .025},
        {"RELIEF / DEPTH", &Theme::relief, 0, 1, .025},
        {"CORNER RADIUS", &Theme::radius, 0, 24, 1},
        {"TEXT SCALE", &Theme::fontScale, .75, 1.5, .01},
    }};
    static bool isSlider(unsigned field) {
        return field >= 200 && field - 200 < sliders.size();
    }
    bool changeSlider(unsigned field, std::optional<double> value, double delta = 0) {
        try {
            auto theme = library_.snapshot().theme;
            const auto& slider = sliders[field - 200];
            theme.*slider.member = std::clamp(value.value_or(theme.*slider.member + delta),
                                              slider.minimum, slider.maximum);
            library_.edit(std::move(theme));
            return true;
        } catch (const std::exception& error) {
            status_ = error.what();
            return false;
        }
    }
    void slide(unsigned field, ui::Rect bounds, double x) {
        if (!std::isfinite(x) || bounds.width <= 10)
            return;
        const auto& slider = sliders[field - 200];
        const auto fraction = std::clamp((x - bounds.x - 5) / (bounds.width - 10), 0., 1.);
        changeSlider(field, slider.minimum + fraction * (slider.maximum - slider.minimum));
    }
    void moveFocus(bool backward) {
        if (focusTargets_.empty())
            return;
        const auto found =
            std::find_if(focusTargets_.begin(), focusTargets_.end(),
                         [this](const auto& target) { return target.field == focusField_; });
        const auto count = focusTargets_.size();
        const auto index = std::size_t(found - focusTargets_.begin());
        const auto& target =
            focusTargets_[found == focusTargets_.end()
                              ? (backward ? count - 1 : 0)
                              : (index + count + (backward ? count - 1 : 1)) % count];
        focusField_ = target.field;
        if (target.scrolls) {
            if (target.bounds.y < clip_.y)
                scroll_ -= clip_.y - target.bounds.y;
            else if (target.bounds.y + target.bounds.height > clip_.y + clip_.height)
                scroll_ += target.bounds.y + target.bounds.height - clip_.y - clip_.height;
            scroll_ = std::clamp(scroll_, 0., maxScroll_);
        }
    }
    static void label(ui::Canvas& c, ui::Rect r, std::string_view text, double size,
                      ui::Color color, bool bold = false) {
        if (r.width <= 0)
            return;
        const double width = c.textWidth(text, size, bold ? 700 : 400);
        if (width > r.width && width > 0)
            size *= r.width / width;
        c.textWeighted({r.x, r.y + r.height * .5 + size * .35}, text, size, color,
                       bold ? 700 : 400);
    }
    void control(ui::Rect r, unsigned field, std::string name, ui::ControlInfo::Role role,
                 double value = 0, double min = 0, double max = 1) {
        focusTargets_.push_back({field, r, clipping_});
        if (clipping_) {
            const double right = std::min(r.x + r.width, clip_.x + clip_.width),
                         bottom = std::min(r.y + r.height, clip_.y + clip_.height);
            r.x = std::max(r.x, clip_.x);
            r.y = std::max(r.y, clip_.y);
            r.width = right - r.x;
            r.height = bottom - r.y;
            if (r.width <= 0 || r.height <= 0)
                return;
        }
        ui::ControlInfo c;
        c.identity = identity(field);
        c.bounds = r;
        c.role = role;
        c.label = c.accessibleName = "THEME / " + name;
        c.enabled = true;
        c.hasValue = role == ui::ControlInfo::Role::slider;
        c.value = value;
        c.minimum = min;
        c.maximum = max;
        controls_.push_back(std::move(c));
    }
    void button(ui::Canvas& c, const Theme& theme, ui::Rect r, unsigned field, std::string text,
                ui::ControlInfo::Role role = ui::ControlInfo::Role::button, bool selected = false) {
        ui::themes::button(c, r, theme, selected, field == focusField_);
        label(c, {r.x + 8, r.y, r.width - 16, r.height}, text, 11, theme.color(Role::ink));
        const auto prior = controls_.size();
        control(r, field,
                field == 20 ? "NAME"
                : field >= 100 && field < 109
                    ? "COLOR " + std::string(ui::themes::roleNames[field - 100])
                    : text,
                role);
        if (controls_.size() > prior)
            controls_.back().valueText = text;
    }
    void commitEntry() {
        if (!entry_)
            return;
        try {
            auto theme = library_.snapshot().theme;
            if (entry_->field == 20)
                theme.name = entry_->text;
            else
                theme.colors[entry_->field - 100] = Theme::parseColor(entry_->text);
            library_.edit(std::move(theme));
            entry_.reset();
            status_ = "USER THEME / UNSAVED PREVIEW";
        } catch (const std::exception& e) {
            status_ = e.what();
        }
    }
    void activate(unsigned field, std::uintptr_t parent) {
        try {
            if (field == 9 || field == 15) {
                close(false);
                return;
            }
            if (field == 20 || (field >= 100 && field < 109)) {
                if (entry_ && entry_->field == field)
                    return;
                commitEntry();
                if (entry_)
                    return;
                const auto theme = library_.snapshot().theme;
                entry_ = Entry{
                    field, field == 20 ? theme.name : Theme::hex(theme.colors[field - 100]), true};
                return;
            }
            commitEntry();
            if (entry_)
                return;
            if (field >= 300 && field < 303) {
                auto theme = library_.snapshot().theme;
                theme.iconSet = iconSetNames[field - 300];
                library_.edit(theme);
                status_ = "ICON SET UPDATED";
            } else if (field >= 310 && field < 313) {
                auto theme = library_.snapshot().theme;
                theme.material = Material(field - 310);
                library_.edit(std::move(theme));
                status_ = "MATERIAL UPDATED";
            } else if (field >= 1000 && field < 1000 + library_.stocks().size()) {
                stockIndex_ = int(field - 1000);
                library_.select(unsigned(stockIndex_));
                status_ = "STOCK THEME / PREVIEW";
            } else if (field == 16)
                close(true);
            else if (field == 14) {
                library_.select(unsigned(stockIndex_));
                status_ = "RESTORED STOCK THEME";
            } else if (field == 13) {
                const auto name = library_.snapshot().theme.name;
                library_.saveNamed(name);
                before_ = library_.snapshot();
                status_ = "SAVED USER THEME";
            } else if (field >= 10 && field <= 12) {
                ui::FileDialog::Request q;
                q.parent = parent;
                q.title = field == 12   ? "Export theme"
                          : field == 10 ? "Load saved user theme"
                                        : "Import .celltheme";
                q.save = field == 12;
                q.extensions = {".celltheme"};
                q.initial = library_.folder() / "User";
                if (dialog_.pending())
                    return;
                if (q.save) {
                    std::filesystem::create_directories(q.initial);
                    const auto filename =
                        File::createLegalFileName(Text(library_.snapshot().theme.name)) +
                        ".celltheme";
                    q.initial = File(q.initial).getChildFile(filename).nativePath();
                }
                if (dialog_.open(std::move(q)))
                    dialogField_ = field;
                else
                    status_ = "SYSTEM FILE DIALOG UNAVAILABLE";
            }
        } catch (const std::exception& e) {
            status_ = e.what();
        }
    }
};
} // namespace cell::ui::themes
