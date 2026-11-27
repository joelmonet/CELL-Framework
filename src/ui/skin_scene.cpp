// SPDX-License-Identifier: Apache-2.0
#include <cell/base64.hpp>
#include <cell/json.hpp>
#include <cell/ui_effects.hpp>
#include <cell/ui_palette.hpp>
#include <cell/ui_skin_scene.hpp>

#include <array>
#include <charconv>
#include <cmath>
#include <list>
#include <numbers>

namespace cell::ui::skins {
namespace {

using Json = json::Value;
constexpr std::array layerNames{"background", "track",     "face",      "pointer",
                                "highlight",  "indicator", "foreground"};
constexpr std::array stateNames{"enabled", "hover", "pressed", "selected", "focus", "disabled"};
constexpr std::array kindNames{"knob",  "slider",   "fader", "button",     "meter",
                               "panel", "dropdown", "tab",   "icon-button"};
enum Layer : unsigned { background, track, face, pointer, highlight, indicator, foreground };
enum class Kind { knob, slider, fader, button, meter, panel, dropdown, tab, iconButton };
using Layers = std::array<std::optional<unsigned>, layerNames.size()>;
constexpr std::size_t layerBudget = 16u * 1024u * 1024u;

[[noreturn]] void invalid(std::string_view detail) {
    throw std::invalid_argument("Skin scene: " + std::string(detail));
}

double number(const Json& field, double minimum, double maximum) {
    const auto* value = field.number();
    if (!value || !std::isfinite(*value) || *value < minimum || *value > maximum)
        invalid("number outside supported range");
    return *value;
}

double optionalNumber(const Json& field, double minimum, double maximum, double fallback) {
    return field.isNull() ? fallback : number(field, minimum, maximum);
}

unsigned index(const Json& field, std::size_t count) {
    if (!count)
        invalid("reference into an empty table");
    const auto value = number(field, 0, double(count - 1));
    if (std::floor(value) != value)
        invalid("reference must be an integer");
    return unsigned(value);
}

Color color(const Json& field) {
    const auto token = field.text();
    if ((token.size() != 7 && token.size() != 9) || token.front() != '#')
        invalid("expected an RGB or RGBA color");
    std::uint32_t rgba{};
    const auto result = std::from_chars(token.data() + 1, token.data() + token.size(), rgba, 16);
    if (result.ec != std::errc{} || result.ptr != token.data() + token.size())
        invalid("invalid color");
    return Color(token.size() == 7 ? 0xff000000u | rgba : (rgba >> 8) | (rgba << 24));
}

const Json::Array& array(const Json& field, std::size_t max) {
    const auto* values = field.array();
    if (!values || values->size() > max)
        invalid("missing or oversized array");
    return *values;
}

const Json::Object& object(const Json& field, std::size_t max) {
    const auto* values = field.object();
    if (!values || values->size() > max)
        invalid("missing or oversized object");
    return *values;
}

unsigned namedIndex(const auto& names, std::string_view name) {
    const auto it = std::find(names.begin(), names.end(), name);
    if (it == names.end())
        invalid("unknown kind, layer or state");
    return unsigned(it - names.begin());
}

struct Material {
    Color fill{0}, end{0}, edge{0}, highlight{0};
    double edgeWidth{}, radius{}, opacity = 1;
    bool ellipse{};
    std::optional<unsigned> asset, mask;
    std::string path;
    double stroke{};
    std::optional<ShadowStyle> shadow, innerShadow, glow;
};

std::optional<ShadowStyle> effect(const Json& field, EffectKind kind) {
    if (field.isNull())
        return {};
    return ShadowStyle{number(field["sigma"], 0, 18),
                       {number(field["x"], -16, 16), number(field["y"], -16, 16)},
                       color(field["color"]),
                       1,
                       kind};
}

struct Control {
    unsigned id{};
    Kind kind{};
    Rect bounds;
    double rotation{}, value{}, indicatorValue{};
    std::string label, display;
    Typeface typeface = Typeface::interfaceText;
    double textSize = 13;
    float weight = 600;
    Layers layers;
    std::array<Layers, stateNames.size()> variations;
    std::array<bool, stateNames.size()> states{};
};

Layers readLayers(const Json& field, std::size_t materials) {
    Layers layers;
    for (const auto& [name, value] : object(field, layerNames.size()))
        layers[namedIndex(layerNames, name)] = index(value, materials);
    return layers;
}

void applyLayers(Layers& to, const Layers& from) {
    for (unsigned i = 0; i < from.size(); ++i)
        if (from[i])
            to[i] = from[i];
}

void shape(Canvas& canvas, Rect rect, const Material& material, Color fill, Color end) {
    const auto radius = std::min(material.radius, std::min(rect.width, rect.height) / 2);
    const std::array stops{GradientStop{0, fill}, GradientStop{1, end}};
    canvas.gradient(rect, stops, {rect.x, rect.y}, {rect.x, rect.y + rect.height}, radius,
                    material.ellipse);
}

void maskPixels(Surface& content, std::span<const std::uint8_t> mask) {
    const int width = content.width(), height = content.height();
    std::vector<std::uint32_t> result(std::size_t(width) * height);
    for (int y = 0; y < height; ++y) {
        const auto* row = reinterpret_cast<const std::uint32_t*>(
            static_cast<const std::byte*>(content.pixels()) + y * content.stride());
        for (int x = 0; x < width; ++x) {
            const auto index = std::size_t(y) * width + x;
            const auto alpha = mask[index];
            std::uint32_t value{};
            for (const unsigned shift : {0u, 8u, 16u, 24u})
                value |= ((((row[x] >> shift) & 255) * alpha + 127) / 255) << shift;
            result[index] = value;
        }
    }
    content.setPixels(result, width, height);
}

struct LayerKey {
    unsigned material{};
    double width{}, height{}, scale{};
    bool operator==(const LayerKey&) const = default;
};
struct CachedMaterial {
    LayerKey key;
    CachedLayer surface;
    std::size_t bytes{};
};

} // namespace

struct Scene::Impl {
    Point dimensions;
    Color backgroundColor{0}, textColor{0};
    std::optional<unsigned> backgroundAsset;
    std::vector<Material> materials;
    std::vector<Control> controls;
    std::array<Control*, 256> byId{};
    std::vector<std::unique_ptr<Surface>> assets;
    std::list<CachedMaterial> layers;
    EffectCache effects{{4u * 1024u * 1024u, 3}};
    std::size_t bytes{};
    std::uint64_t builds{};

    explicit Impl(std::string_view text) {
        const auto root = json::read(text, {Scene::maxFileBytes, 131072, 12u * 1024u * 1024u, 10});
        if (root["schema"].text() != "cell.ui-skin-scene.v1")
            invalid("unsupported schema");
        dimensions = {number(root["width"], 1, 4096), number(root["height"], 1, 4096)};
        backgroundColor = color(root["colors"]["background"]);
        textColor = color(root["colors"]["text"]);
        loadAssets(array(root["assets"], 48));
        if (!root["backgroundAsset"].isNull())
            backgroundAsset = index(root["backgroundAsset"], assets.size());
        for (const auto& value : array(root["materials"], 4096))
            materials.push_back(readMaterial(value));
        const auto& items = array(root["controls"], 256);
        controls.reserve(items.size());
        for (const auto& value : items) {
            auto control = readControl(value);
            if (byId[control.id])
                invalid("duplicate control ID");
            controls.push_back(std::move(control));
            byId[controls.back().id] = &controls.back();
        }
    }

    void loadAssets(const Json::Array& entries) {
        constexpr std::array pngSignature{std::byte{137}, std::byte{80}, std::byte{78},
                                          std::byte{71},  std::byte{13}, std::byte{10},
                                          std::byte{26},  std::byte{10}};
        std::size_t remaining = 6u * 1024u * 1024u; // Decoded pixels, not encoded bytes.
        for (const auto& entry : entries) {
            const auto encoded = encoding::decodeBase64(entry.text(), 2500002);
            if (!encoded || encoded->size() < pngSignature.size() ||
                !std::equal(pngSignature.begin(), pngSignature.end(), encoded->begin()))
                invalid("expected a prepared PNG asset");
            auto decoded = decodeImage(*encoded, std::min<std::size_t>(remaining, 768u * 768u));
            remaining -= decoded.pixels.size();
            auto surface = std::make_unique<Surface>();
            surface->setPixels(decoded.pixels, decoded.width, decoded.height);
            assets.push_back(std::move(surface));
        }
    }

    Material readMaterial(const Json& value) const {
        Material m;
        m.fill = color(value["fill"]);
        m.end = color(value["end"]);
        m.edge = color(value["edge"]);
        m.highlight = color(value["highlight"]);
        m.edgeWidth = number(value["edgeWidth"], 0, 6);
        m.radius = number(value["radius"], 0, 48);
        m.opacity = number(value["opacity"], 0, 1);
        const auto geometry = value["shape"].text();
        if (geometry != "ellipse" && geometry != "rounded")
            invalid("invalid corner geometry");
        m.ellipse = geometry == "ellipse";
        if (!value["asset"].isNull())
            m.asset = index(value["asset"], assets.size());
        if (!value["mask"].isNull())
            m.mask = index(value["mask"], assets.size());
        if (!value["vector"].isNull()) {
            const auto path = value["vector"]["path"].text();
            if (path.empty() || path.size() > 1024 ||
                path.find_first_not_of("MLHVCSAZeE0123456789+., -") != path.npos)
                invalid("invalid vector path");
            m.path = path;
            m.stroke = number(value["vector"]["stroke"], .1, 6);
        }
        m.shadow = effect(value["shadow"], EffectKind::shadow);
        m.innerShadow = effect(value["innerShadow"], EffectKind::innerShadow);
        m.glow = effect(value["glow"], EffectKind::shadow);
        return m;
    }

    Control readControl(const Json& value) const {
        Control c;
        c.id = index(value["id"], byId.size());
        c.kind = Kind(namedIndex(kindNames, value["kind"].text()));
        const auto& rect = array(value["rect"], 4);
        if (rect.size() != 4)
            invalid("invalid control rectangle");
        c.bounds = {number(rect[0], 0, dimensions.x), number(rect[1], 0, dimensions.y),
                    number(rect[2], .25, dimensions.x), number(rect[3], .25, dimensions.y)};
        if (c.bounds.x + c.bounds.width > dimensions.x ||
            c.bounds.y + c.bounds.height > dimensions.y)
            invalid("out-of-bounds control");
        c.value = number(value["value"], 0, 1);
        c.rotation = optionalNumber(value["rotation"], -360, 360, 0);
        c.indicatorValue = optionalNumber(value["indicator"], 0, 1, 0);
        c.label = value["label"].text();
        c.display = value["display"].text();
        if (c.display.size() > 1024)
            invalid("control display text too long");
        if (c.label.size() > 1024)
            invalid("control label too long");
        const auto& type = value["typography"];
        const auto family = type["family"].text();
        if (family == "panel-condensed")
            c.typeface = Typeface::brandCondensed;
        else if (family == "system-mono")
            c.typeface = Typeface::monospace;
        else if (family == "panel-sans")
            c.typeface = Typeface::interfaceText;
        c.textSize = optionalNumber(type["size"], 8, 96, 13);
        c.weight = float(optionalNumber(type["weight"], 100, 900, 600));
        c.layers = readLayers(value["layers"], materials.size());
        for (const auto& [name, variation] : object(value["states"], stateNames.size()))
            c.variations[namedIndex(stateNames, name)] = readLayers(variation, materials.size());
        c.states[unsigned(State::enabled)] = true;
        if (const auto* selected = value["selected"].boolean())
            c.states[unsigned(State::selected)] = *selected;
        if (const auto* disabled = value["disabled"].boolean())
            c.states[unsigned(State::disabled)] = *disabled;
        return c;
    }

    void faceContent(Canvas& canvas, Rect rect, const Material& m) const {
        if (!m.path.empty()) {
            if (!canvas.svgPath(m.path, rect, m.fill, m.stroke))
                invalid("unsupported vector path");
            return;
        }
        shape(canvas, rect, m, m.fill, m.end);
        if (m.asset) {
            const auto& image = *assets[*m.asset];
            const double scale = std::min(rect.width / image.width(), rect.height / image.height());
            const double width = image.width() * scale, height = image.height() * scale;
            canvas.image(image, {rect.x + (rect.width - width) / 2,
                                 rect.y + (rect.height - height) / 2, width, height});
        }
        const auto radius = std::min(m.radius, std::min(rect.width, rect.height) / 2);
        if (m.edgeWidth) {
            const auto inner = rect.inset(m.edgeWidth / 2);
            if (m.ellipse)
                canvas.ellipse(inner, m.edge, m.edgeWidth);
            else
                canvas.strokeRounded(inner, radius, m.edge, m.edgeWidth);
        }
        if (m.highlight.argb >> 24) {
            if (m.ellipse) {
                canvas.ellipse(rect.inset(1), m.highlight, 1);
            } else {
                const double x = std::max(1., radius / 2);
                canvas.line({rect.x + x, rect.y + 1}, {rect.x + rect.width - x, rect.y + 1},
                            m.highlight);
            }
        }
    }

    void buildMaterial(Canvas& canvas, Rect rect, const Material& m, double scale) {
        constexpr Color white(0xffffffff);
        const int width = int(std::ceil(rect.width * scale));
        const int height = int(std::ceil(rect.height * scale));
        AlphaMask mask;
        if (m.shadow || m.innerShadow || m.glow || m.mask || m.asset) {
            mask.rasterize(width, height, [&](Canvas& offscreen) {
                offscreen.scale(scale, scale);
                if (m.mask)
                    offscreen.image(*assets[*m.mask], {0, 0, rect.width, rect.height});
                else
                    shape(offscreen, {0, 0, rect.width, rect.height}, m, white, white);
            });
        }
        if (m.shadow)
            effects.draw(canvas, mask, {rect.x, rect.y}, *m.shadow, scale);
        if (m.glow)
            effects.draw(canvas, mask, {rect.x, rect.y}, *m.glow, scale);
        if (m.mask || m.asset) {
            Surface content;
            content.setPixels(std::vector<std::uint32_t>(std::size_t(width) * height), width,
                              height);
            content.render([&](Canvas& offscreen) {
                offscreen.scale(scale, scale);
                faceContent(offscreen, {0, 0, rect.width, rect.height}, m);
                if (m.innerShadow)
                    effects.draw(offscreen, mask, {0, 0}, *m.innerShadow, scale);
            });
            maskPixels(content, mask.pixels());
            canvas.image(content, {rect.x, rect.y, width / scale, height / scale});
        } else {
            faceContent(canvas, rect, m);
            if (m.innerShadow)
                effects.draw(canvas, mask, {rect.x, rect.y}, *m.innerShadow, scale);
        }
    }

    void drawLayer(Canvas& canvas, std::optional<unsigned> which, Rect rect, double scale) {
        if (!which || rect.width <= 0 || rect.height <= 0)
            return;
        const auto& m = materials[*which];
        if (m.opacity == 0)
            return;
        const auto pad = (m.shadow || m.glow)
                             ? std::max(m.shadow ? graphics::blurPadding(m.shadow->sigma) +
                                                       std::max(std::abs(m.shadow->offset.x),
                                                                std::abs(m.shadow->offset.y))
                                                 : 0.,
                                        m.glow ? graphics::blurPadding(m.glow->sigma) +
                                                     std::max(std::abs(m.glow->offset.x),
                                                              std::abs(m.glow->offset.y))
                                               : 0.) +
                                   2
                             : 0.;
        const Rect padded{rect.x - pad, rect.y - pad, rect.width + 2 * pad, rect.height + 2 * pad};
        const LayerKey key{*which, rect.width, rect.height, scale};
        const auto cost = std::ceil(padded.width * scale) * std::ceil(padded.height * scale) * 4;
        canvas.save();
        canvas.opacity(m.opacity);
        if (cost > layerBudget) {
            // A background larger than the material budget is still drawable.
            // Cap its intermediate raster work and cache it at a lower DPI.
            const auto reduced = std::min(
                scale,
                std::floor(std::sqrt(layerBudget / (4 * padded.width * padded.height)) * 4) / 4);
            canvas.restore();
            if (reduced >= .25 && reduced < scale)
                drawLayer(canvas, which, rect, reduced);
            return;
        }
        const auto cached = std::find_if(layers.begin(), layers.end(),
                                         [&](const auto& entry) { return entry.key == key; });
        if (cached != layers.end()) {
            layers.splice(layers.begin(), layers, cached);
            static const std::function<void(Canvas&)> noPaint;
            layers.front().surface.draw(canvas, padded, scale, 1, noPaint);
        } else {
            while (!layers.empty() && (layers.size() >= 64 || bytes + cost > layerBudget)) {
                bytes -= layers.back().bytes;
                layers.pop_back();
            }
            CachedMaterial next;
            next.key = key;
            next.surface.draw(canvas, padded, scale, 1, [&](Canvas& offscreen) {
                buildMaterial(offscreen, {pad, pad, rect.width, rect.height}, m, scale);
            });
            next.bytes = next.surface.retainedBytes();
            bytes += next.bytes;
            layers.push_front(std::move(next));
            ++builds;
        }
        canvas.restore();
    }

    void paintControl(Canvas& canvas, const Control& control, double scale) {
        if (control.rotation) {
            canvas.save();
            const auto& r = control.bounds;
            const auto x = r.x + r.width / 2, y = r.y + r.height / 2;
            canvas.translate(x, y);
            canvas.rotate(control.rotation * std::numbers::pi / 180);
            canvas.translate(-x, -y);
        }
        auto layersNow = control.layers;
        for (unsigned i = 0; i < control.states.size(); ++i)
            if (control.states[i])
                applyLayers(layersNow, control.variations[i]);
        const auto rect = control.bounds;
        const auto draw = [&](Layer layer, Rect bounds) {
            drawLayer(canvas, layersNow[layer], bounds, scale);
        };
        draw(background, rect);
        const Rect content{rect.x + 12, rect.y + 29, std::max(0., rect.width - 24),
                           std::max(0., rect.height - 39)};
        const auto centerX = content.x + content.width / 2,
                   centerY = content.y + content.height / 2;
        if (!control.label.empty())
            canvas.textWeighted({rect.x + 12, rect.y + 21}, control.label, control.textSize,
                                textColor, control.weight, control.typeface);
        switch (control.kind) {
        case Kind::knob: {
            const auto diameter = std::min({content.width, content.height, 118.});
            const Rect dial{centerX - diameter / 2, centerY - diameter / 2, diameter, diameter};
            draw(track, dial.inset(-4));
            draw(face, dial);
            draw(highlight, dial.inset(diameter * .08));
            canvas.save();
            canvas.translate(centerX, centerY);
            canvas.rotate((control.value - .5) * 1.5 * std::numbers::pi);
            draw(pointer, {-2, -diameter * .39, 4, diameter * .20});
            canvas.restore();
            if (layersNow[indicator] && control.indicatorValue > 0 && diameter >= 20) {
                const auto color = materials[*layersNow[indicator]].fill;
                const double radius = diameter / 2 + 6;
                constexpr double start = std::numbers::pi * .75;
                const double sweep = std::numbers::pi * 1.5 * control.indicatorValue;
                std::array<Point, 24> arc;
                for (unsigned i = 0; i < arc.size(); ++i) {
                    const double angle = start + sweep * i / (arc.size() - 1);
                    arc[i] = {centerX + std::cos(angle) * radius,
                              centerY + std::sin(angle) * radius};
                }
                canvas.polyline(arc, color, 2);
            }
            break;
        }
        case Kind::fader: {
            const double length = std::max(0., content.height - 8);
            draw(track, {centerX - 6, content.y + 4, 12, length});
            const auto top = content.y + (1 - control.value) * length;
            const Rect cap{centerX - 21, top, 42, 16};
            draw(face, cap);
            draw(pointer, {cap.x + 5, cap.y + 7, 32, 2});
            break;
        }
        case Kind::slider:
        case Kind::meter: {
            const Rect rail{content.x, centerY - 7, content.width, 14};
            draw(track, rail);
            canvas.save();
            canvas.clip({rail.x, rail.y, rail.width * control.value, rail.height});
            draw(indicator, rail);
            canvas.restore();
            if (control.kind == Kind::slider)
                draw(face, {rail.x + rail.width * control.value - 7, rail.y - 2, 14, 18});
            break;
        }
        case Kind::panel:
            draw(highlight, rect.inset(2));
            break;
        default:
            draw(face, content);
            if (!control.display.empty())
                canvas.textWeighted({content.x + 6, centerY + 4}, control.display, control.textSize,
                                    textColor, control.weight, control.typeface);
            draw(highlight, content.inset(2));
            if (control.kind == Kind::tab)
                draw(indicator, {content.x, content.y + content.height - 4, content.width, 3});
            if (control.kind == Kind::dropdown || control.kind == Kind::iconButton) {
                const Rect icon{content.x + content.width - 31, centerY - 10, 22, 20};
                draw(pointer, icon);
            }
            break;
        }
        draw(foreground, rect);
        if (control.rotation)
            canvas.restore();
    }
};

Scene::Scene(std::string_view text) : impl_(std::make_unique<Impl>(text)) {}
Scene::~Scene() = default;
Scene::Scene(Scene&&) noexcept = default;
Scene& Scene::operator=(Scene&&) noexcept = default;

Point Scene::size() const noexcept {
    return impl_->dimensions;
}

std::optional<Rect> Scene::controlBounds(unsigned id) const noexcept {
    if (id >= impl_->byId.size() || !impl_->byId[id])
        return {};
    return impl_->byId[id]->bounds;
}

bool Scene::setValue(unsigned id, double value) noexcept {
    if (!controlBounds(id) || !std::isfinite(value) || value < 0 || value > 1)
        return false;
    impl_->byId[id]->value = value;
    return true;
}

bool Scene::setIndicator(unsigned id, double value) noexcept {
    if (!controlBounds(id) || !std::isfinite(value) || value < 0 || value > 1)
        return false;
    impl_->byId[id]->indicatorValue = value;
    return true;
}

bool Scene::setState(unsigned id, State state, bool active) noexcept {
    if (!controlBounds(id) || unsigned(state) >= stateNames.size())
        return false;
    impl_->byId[id]->states[unsigned(state)] = active;
    return true;
}

void Scene::paint(Canvas& canvas, double scale) {
    if (!std::isfinite(scale) || scale < .25 || scale > 4)
        invalid("unsupported device scale");
    PaletteScope exactColors(std::span<const PaletteReplacement>{});
    const auto [width, height] = impl_->dimensions;
    const Rect bounds{0, 0, width, height};
    canvas.fill(bounds, impl_->backgroundColor);
    if (impl_->backgroundAsset)
        canvas.image(*impl_->assets[*impl_->backgroundAsset], bounds);
    for (const auto& control : impl_->controls)
        impl_->paintControl(canvas, control, scale);
}

CacheStats Scene::cacheStats() const noexcept {
    return {impl_->layers.size(), impl_->bytes, impl_->builds, impl_->effects.stats().blurBuilds};
}

} // namespace cell::ui::skins
