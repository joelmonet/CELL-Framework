// SPDX-License-Identifier: Apache-2.0
#pragma once
#include <algorithm>
#include <array>
#include <cell/files.hpp>
#include <cell/json.hpp>
#include <cell/ui.hpp>
#include <cell/ui_palette.hpp>
#include <charconv>
#include <cmath>
#include <filesystem>
#include <string>
namespace cell::ui::themes {
enum class Material { classic, sculpted, modern };
enum class Role : unsigned {
    background,
    panel,
    surface,
    edge,
    ink,
    muted,
    accent,
    graph,
    gold,
    count
};
inline constexpr json::Limits themeLimits{65536, 512, 32768, 8};
inline constexpr std::array<std::string_view, 3> materialNames{"classic", "sculpted", "modern"};
inline constexpr std::array<std::string_view, 3> iconSetNames{"classic", "hdr", "modern"};
inline constexpr std::array<std::string_view, unsigned(Role::count)> roleNames{
    "background", "panel", "surface", "edge", "ink", "muted", "accent", "graph", "gold"};
struct Theme {
    static constexpr std::size_t maxNameBytes = 96;
    std::string name = "Untitled", iconSet = "classic";
    Material material = Material::classic;
    std::array<Color, roleNames.size()> colors{
        Color(0xff13282d), Color(0xff243138), Color(0xff3b4e56),
        Color(0xff586f73), Color(0xffd8e3df), Color(0xff91a7a8),
        Color(0xff91ced4), Color(0xff13282d), Color(0xffefcb78)};
    double glow = .3, texture = 0, relief = .3, radius = 4, fontScale = 1;
    json::Value::Object extensions;
    Color color(Role r) const noexcept {
        return colors[unsigned(r)];
    }
    bool operator==(const Theme& other) const {
        return serialize() == other.serialize();
    }
    static std::string hex(Color color) {
        char value[10];
        value[0] = '#';
        constexpr char digits[] = "0123456789ABCDEF";
        for (unsigned i = 0; i < 8; ++i)
            value[i + 1] = digits[(color.argb >> (28 - i * 4)) & 15];
        return {value, 9};
    }
    static Color parseColor(std::string_view value) {
        if (!value.empty() && value.front() == '#')
            value.remove_prefix(1);
        if (value.size() != 6 && value.size() != 8)
            throw std::invalid_argument("Use #RRGGBB or #AARRGGBB");
        std::uint32_t out{};
        const auto result = std::from_chars(value.data(), value.data() + value.size(), out, 16);
        if (result.ec != std::errc{} || result.ptr != value.data() + value.size())
            throw std::invalid_argument("Invalid theme color");
        if (value.size() == 6)
            out |= 0xff000000;
        return Color(out);
    }
    void validate() const {
        if (name.empty() || name.size() > maxNameBytes ||
            std::any_of(name.begin(), name.end(),
                        [](unsigned char ch) { return ch < 32 || ch == 127; }))
            throw std::invalid_argument("Theme name must be 1–96 UTF-8 bytes on one line");
        for (auto cursor = Text::Codepoints(name, 0), end = Text::Codepoints(name, name.size());
             cursor != end;) {
            const auto offset = cursor.bytePosition();
            const auto codepoint = *cursor;
            ++cursor;
            if (codepoint == 0x85 || codepoint == 0x2028 || codepoint == 0x2029 ||
                (codepoint == 0xfffd && cursor.bytePosition() - offset != 3))
                throw std::invalid_argument("Theme name must be valid UTF-8 on one line");
        }
        if (std::find(iconSetNames.begin(), iconSetNames.end(), iconSet) == iconSetNames.end())
            throw std::invalid_argument("Unknown icon set");
        if (unsigned(material) >= materialNames.size())
            throw std::invalid_argument("Unknown theme material");
        for (double v : {glow, texture, relief})
            if (!std::isfinite(v) || v < 0 || v > 1)
                throw std::invalid_argument("Theme effect must be between 0 and 1");
        if (!std::isfinite(radius) || radius < 0 || radius > 24 || !std::isfinite(fontScale) ||
            fontScale < .75 || fontScale > 1.5)
            throw std::invalid_argument("Invalid theme radius or text scale");
    }
    std::string serialize() const {
        validate();
        using V = json::Value;
        V::Object object = extensions, palette;
        for (unsigned i = 0; i < colors.size(); ++i)
            palette[std::string(roleNames[i])] = V(hex(colors[i]));
        object["schema"] = V("cell.ui-theme.v1");
        object["iconSet"] = V(iconSet);
        object["name"] = V(name);
        object["material"] = V(materialNames[unsigned(material)]);
        object["colors"] = V(std::move(palette));
        object["glow"] = V(glow);
        object["texture"] = V(texture);
        object["relief"] = V(relief);
        object["radius"] = V(radius);
        object["fontScale"] = V(fontScale);
        return json::write(V(std::move(object)), themeLimits);
    }
    static Theme read(std::string_view text) {
        const auto value = json::read(text, themeLimits);
        if (!value.isObject() || value["schema"].text() != "cell.ui-theme.v1")
            throw std::invalid_argument("Unsupported .celltheme schema");
        Theme out;
        out.extensions = *value.object();
        const auto* name = value["name"].string();
        if (!name)
            throw std::invalid_argument("Theme name is missing");
        out.name = *name;
        if (!value["iconSet"].isNull()) {
            const auto* icon = value["iconSet"].string();
            if (!icon)
                throw std::invalid_argument("Invalid theme icon set");
            out.iconSet = *icon;
        }
        const auto material = value["material"].text();
        if (material == "classic")
            out.material = Material::classic;
        else if (material == "sculpted")
            out.material = Material::sculpted;
        else if (material == "modern")
            out.material = Material::modern;
        else
            throw std::invalid_argument("Unsupported theme material");
        for (unsigned i = 0; i < roleNames.size(); ++i) {
            const auto* color = value["colors"][roleNames[i]].string();
            if (!color)
                throw std::invalid_argument("Theme color is missing");
            out.colors[i] = parseColor(*color);
        }
        const auto number = [&](const char* name, double& target) {
            if (const auto* v = value[name].number())
                target = *v;
            else if (!value[name].isNull())
                throw std::invalid_argument("Invalid numeric theme token");
        };
        number("glow", out.glow);
        number("texture", out.texture);
        number("relief", out.relief);
        number("radius", out.radius);
        number("fontScale", out.fontScale);
        for (const auto* key : {"schema", "name", "iconSet", "material", "colors", "glow",
                                "texture", "relief", "radius", "fontScale"})
            out.extensions.erase(key);
        out.validate();
        return out;
    }
    static Theme load(const std::filesystem::path& file) {
        const auto bytes = files::readBounded(file, themeLimits.inputBytes);
        return read({reinterpret_cast<const char*>(bytes.data()), bytes.size()});
    }
    void exportFile(std::filesystem::path file) const {
        if (file.extension() != ".celltheme")
            file.replace_extension(".celltheme");
        const auto text = serialize();
        files::atomicWrite(file, std::as_bytes(std::span(text)), true);
    }
};
inline Color alpha(Color color, double opacity) {
    return Color((color.argb & 0xffffffu) |
                 (std::uint32_t(std::clamp(opacity, 0., 1.) * 255) << 24));
}
inline Color mix(Color a, Color b, double amount) {
    amount = std::clamp(amount, 0., 1.);
    std::uint32_t value = 0;
    for (unsigned shift : {0u, 8u, 16u, 24u})
        value |= std::uint32_t(((a.argb >> shift) & 255) * (1 - amount) +
                               ((b.argb >> shift) & 255) * amount + .5)
                 << shift;
    return Color(value);
}
// These functions paint a skin only. They own no parameter, gesture or audio state.
inline void panel(Canvas& c, Rect r, const Theme& theme) {
    const auto base = theme.color(Role::panel), edge = theme.color(Role::edge);
    const double radius = theme.radius;
    if (theme.material == Material::modern) {
        c.gradient(r, base, mix(base, Color(0xff000000), theme.relief * .12), radius);
    } else {
        c.rounded({r.x, r.y + 3, r.width, r.height}, radius, Color(0x66000000));
        c.gradient(r, mix(base, Color(0xffffffff), .025 + theme.relief * .035),
                   mix(base, Color(0xff000000), .08 + theme.relief * .2), radius);
    }
    if (theme.material == Material::sculpted) {
        c.strokeRounded(r.inset(.5), radius, alpha(theme.color(Role::gold), .5), 1);
        c.strokeRounded(r.inset(3), std::max(1., radius - 2), alpha(theme.color(Role::gold), .22),
                        1);
        c.strokeRounded(r.inset(5), std::max(1., radius - 4), Color(0xbb000000), 2);
    } else
        c.strokeRounded(r.inset(.5), radius, alpha(edge, .7), 1);
    if (theme.texture > .005 && r.width > 12 && r.height > 12) {
        c.save();
        c.clip(r.inset(6));
        for (unsigned n = 0; n < 72; ++n) {
            const double y = r.y + 6 + (r.height - 12) * n / 71.;
            c.line({r.x + 6, y}, {r.x + r.width - 6, y},
                   alpha(n % 2 ? Color(0xffffffff) : Color(0xff000000),
                         theme.texture * (n % 2 ? .017 : .045)),
                   .6);
        }
        c.restore();
    }
}
// GUI-only cache of static panel layers. Live graphs and controls paint above
// these backplates. Bound memory; oversized or actively changing sizes can use
// the direct panel() path. Device scale participates in the cache key.
class PanelCache {
public:
    explicit PanelCache(std::size_t budget = 8u * 1024 * 1024) : budget_(budget) {}
    void theme(const Theme& theme, std::uint64_t revision) {
        if (revision_ == revision && initialized_)
            return;
        theme.validate();
        theme_ = theme;
        revision_ = revision;
        initialized_ = true;
        clear();
    }
    void clear() {
        entries_.clear();
        bytes_ = 0;
    }
    std::size_t bytes() const noexcept {
        return bytes_;
    }
    void paint(Canvas& canvas, Rect r, double scale = 1) {
        if (!initialized_)
            throw std::logic_error("Set theme before painting cached panels");
        if (!std::isfinite(scale) || scale <= 0 || !std::isfinite(r.width) ||
            !std::isfinite(r.height) || r.width <= 0 || r.height <= 0)
            return;
        PaletteScope exactColors(std::span<const PaletteReplacement>{});
        Rect layer = r;
        layer.height += 3; // Include the panel's drop shadow.
        const auto width = std::ceil(layer.width * scale), height = std::ceil(layer.height * scale);
        if (scale > 4 || width > 4096 || height > 4096 || width * height * 4 > double(budget_)) {
            panel(canvas, r, theme_);
            return;
        }
        const auto w = int(width), h = int(height);
        for (auto& entry : entries_)
            if (entry.logicalWidth == r.width && entry.logicalHeight == r.height &&
                entry.scale == scale) {
                entry.age = ++age_;
                canvas.image(*entry.surface, layer);
                return;
            }
        const auto cost = std::size_t(w) * std::size_t(h) * 4;
        while (!entries_.empty() && (bytes_ + cost > budget_ || entries_.size() >= 12)) {
            const auto old =
                std::min_element(entries_.begin(), entries_.end(),
                                 [](const auto& a, const auto& b) { return a.age < b.age; });
            bytes_ -= std::size_t(old->width) * old->height * 4;
            entries_.erase(old);
        }
        auto surface = std::make_unique<Surface>();
        surface->resize(w, h);
        surface->render([&](Canvas& c) {
            c.scale(w / layer.width, h / layer.height);
            panel(c, {0, 0, r.width, r.height}, theme_);
        });
        entries_.push_back({w, h, r.width, r.height, scale, ++age_, std::move(surface)});
        bytes_ += cost;
        canvas.image(*entries_.back().surface, layer);
    }

private:
    struct Entry {
        int width{}, height{};
        double logicalWidth{}, logicalHeight{}, scale = 1;
        std::uint64_t age{};
        std::unique_ptr<Surface> surface;
    };
    Theme theme_;
    std::uint64_t revision_{}, age_{};
    bool initialized_{};
    std::size_t budget_{}, bytes_{};
    std::vector<Entry> entries_;
};
inline void button(Canvas& c, Rect r, const Theme& theme, bool active = false, bool hovered = false,
                   bool down = false) {
    const auto face = r.inset(1);
    const auto surface = theme.color(Role::surface), accent = theme.color(Role::accent);
    const double radius = std::min(theme.radius, std::max(0., face.height * .4));
    if (active || hovered)
        for (unsigned n = 3; n > 0; --n)
            c.strokeRounded({face.x - n, face.y - n, face.width + 2 * n, face.height + 2 * n},
                            radius + n, alpha(accent, theme.glow * (active ? .08 : .04)), 1.5);
    c.rounded({face.x, face.y + (down ? 0 : 2), face.width, face.height}, radius,
              Color(0x80000000));
    c.gradient(face, mix(surface, Color(0xffffffff), down ? 0 : .05 + theme.relief * .08),
               mix(surface, Color(0xff000000), down ? .3 : .2), radius);
    c.strokeRounded(face.inset(.5), radius,
                    active || hovered
                        ? accent
                        : alpha(theme.material == Material::sculpted ? theme.color(Role::gold)
                                                                     : theme.color(Role::edge),
                                .75),
                    active ? 1.4 : 1.);
    if (active)
        c.line({face.x + 6, face.y + face.height - 2},
               {face.x + face.width - 6, face.y + face.height - 2}, accent, 1.2);
}
inline void halo(Canvas& c, Point center, double radius, const Theme& theme, bool active) {
    if (!active || theme.glow <= 0)
        return;
    for (unsigned n = 4; n > 0; --n)
        c.strokeCircle(center, radius + n, alpha(theme.color(Role::accent), theme.glow * .06), 2);
}
} // namespace cell::ui::themes
