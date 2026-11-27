// SPDX-License-Identifier: Apache-2.0
#include <cell/base64.hpp>
#include <cell/json.hpp>
#include <cell/ui_effects.hpp>
#include <cell/ui_palette.hpp>
#include <cell/ui_skin_scene.hpp>
#include <cell/ui_sliced_artwork.hpp>

#include <array>
#include <charconv>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <list>
#include <numbers>

namespace cell::ui::skins {
namespace {

using Json = json::Value;
constexpr std::array layerNames{"background", "track",     "face",      "pointer",
                                "highlight",  "indicator", "foreground"};
constexpr std::array stateNames{"enabled",   "hover",    "pressed",  "selected",
                                "focus",     "disabled", "bypassed", "automated",
                                "modulated", "invalid",  "warning"};
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

struct Gradient {
    GradientKind type = GradientKind::solid;
    double angle{};
    Point center;
    std::vector<GradientStop> stops;
};
struct Border {
    Color color{0};
    double width{};
};
enum class TextureFit { stretch, contain, cover, tile };
struct Texture {
    unsigned asset{};
    TextureFit fit = TextureFit::contain;
    PaintBlend blend = PaintBlend::normal;
    double scale = 1, opacity = 1;
    Point offset;
};
struct Bevel {
    double width{}, angle{};
    Color light{0}, dark{0};
};

template <std::size_t Size>
std::array<double, Size> numbers(const Json& field, double minimum, double maximum) {
    const auto& values = array(field, Size);
    if (values.size() != Size)
        invalid("invalid vector size");
    std::array<double, Size> result;
    for (std::size_t i = 0; i < Size; ++i)
        result[i] = number(values[i], minimum, maximum);
    return result;
}

Color tintAlpha(Color color, double opacity) {
    return Color((color.argb & 0xffffffu) |
                 (std::uint32_t(std::lround((color.argb >> 24) * opacity)) << 24));
}

FillGeometry fillGeometry(Rect rect, GradientKind kind, double angle, Point center = {.5, .5}) {
    constexpr double degrees = std::numbers::pi / 180.;
    const Point origin{rect.x + rect.width * center.x, rect.y + rect.height * center.y};
    if (kind == GradientKind::radial) {
        const auto farX = std::max(origin.x - rect.x, rect.x + rect.width - origin.x);
        const auto farY = std::max(origin.y - rect.y, rect.y + rect.height - origin.y);
        return {kind, origin, {origin.x + std::hypot(farX, farY), origin.y}, 0};
    }
    if (kind == GradientKind::conic)
        return {kind, origin, {}, (angle - 90) * degrees};
    const Point dir{std::sin(angle * degrees), -std::cos(angle * degrees)};
    const double radius = (std::abs(dir.x) * rect.width + std::abs(dir.y) * rect.height) / 2;
    return {kind,
            {origin.x - dir.x * radius, origin.y - dir.y * radius},
            {origin.x + dir.x * radius, origin.y + dir.y * radius},
            0};
}

Gradient readGradient(const Json& value) {
    constexpr std::array types{"solid", "linear", "radial", "conic"};
    const auto type = GradientKind(namedIndex(types, value["kind"].text()));
    const auto center = numbers<2>(value["center"], 0, 1);
    Gradient out{type, number(value["angle"], -360, 360), {center[0], center[1]}, {}};
    const auto& stops = array(value["stops"], 16);
    if (type == GradientKind::solid ? stops.size() != 1 : stops.size() < 2)
        invalid("invalid gradient stop count");
    double last = -1;
    for (const auto& stop : stops) {
        const auto at = number(stop["at"], 0, 1);
        if (at < last)
            invalid("out-of-order gradient stops");
        out.stops.push_back({at, tintAlpha(color(stop["color"]), number(stop["opacity"], 0, 1))});
        last = at;
    }
    return out;
}

struct Material {
    Color fill{0}, end{0}, edge{0}, highlight{0};
    double edgeWidth{}, radius{}, opacity = 1;
    bool ellipse{};
    std::optional<unsigned> asset, mask;
    std::string path;
    double stroke{};
    Rect viewBox{0, 0, 24, 24};
    Point iconOffset;
    std::optional<ShadowStyle> shadow, innerShadow, glow;
    std::optional<Gradient> gradient;
    std::optional<ArtworkSlices> slices;
    std::array<double, 4> corners{};
    std::vector<Border> borders;
    std::optional<Texture> texture;
    std::optional<Bevel> bevel;
    double ambient{}, lightDirection{}, lightIntensity{}, sheen{};
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

enum class LayerMotion { rotate, x, y, fillX, fillY, opacity };
struct StackLayer {
    unsigned material{};
    Rect rect;
    Point anchor, pivot, offset, scale{1, 1};
    double angle{}, opacity = 1, firstVisible{}, lastVisible = 1;
    std::optional<unsigned> state;
    std::optional<LayerMotion> motion;
    double startMotion{}, endMotion{};
    bool fromIndicator{}, clip{}, horizontalFrames{};
    unsigned frames{};
    std::string text;
};
struct ControlGeometry {
    double firstAngle = -135, lastAngle = 135;
    Point pivot{.5, .5};
    bool rotateCap{}, vertical{};
    unsigned ticks{};
    enum class Arc { none, unipolar, bipolar } arc = Arc::none;
    double origin{};
    std::optional<double> unity;
};

constexpr std::array analysisNames{"spectrum", "frequency-response", "transfer",   "gain-reduction",
                                   "waveform", "meter-track",        "meter-fill", "peak-hold",
                                   "handle",   "selected-band",      "mid",        "side",
                                   "sidechain"};
constexpr unsigned maxTracePoints = 2048;
enum class TraceMode { line, fill, points };
struct Trace {
    Color color{0};
    double width = 1;
    TraceMode mode = TraceMode::line;
    bool available{};
    std::vector<Point> points;
};
struct Analysis {
    Color background{0}, major{0}, minor{0}, labels{0};
    unsigned columns{}, rows{}, subdivisions{}, precision{};
    double minimum{}, maximum{};
    std::string unit;
    std::array<Trace, analysisNames.size()> traces;
};

Analysis readAnalysis(const Json& value) {
    Analysis analysis;
    analysis.background = color(value["background"]);
    analysis.major = color(value["major"]);
    analysis.minor = color(value["minor"]);
    analysis.labels = color(value["labels"]);
    const auto grid = numbers<3>(value["grid"], 1, 16);
    if (std::any_of(grid.begin(), grid.end(), [](double n) { return n != std::floor(n); }))
        invalid("analysis grid requires integer counts");
    analysis.columns = unsigned(grid[0]);
    analysis.rows = unsigned(grid[1]);
    analysis.subdivisions = unsigned(grid[2]);
    const auto range = numbers<2>(value["axes"]["range"], -1e9, 1e9);
    if (range[0] >= range[1])
        invalid("analysis axis range is empty");
    analysis.minimum = range[0];
    analysis.maximum = range[1];
    analysis.precision = index(value["axes"]["precision"], 7);
    analysis.unit = value["axes"]["unit"].text();
    if (analysis.unit.size() > 96)
        invalid("analysis axis unit too long");
    constexpr std::array modes{"line", "fill", "points"};
    for (const auto& [role, paint] : object(value["traces"], analysisNames.size())) {
        auto& trace = analysis.traces[namedIndex(analysisNames, role)];
        trace.width = number(paint["width"], .5, 8);
        trace.color = color(paint["color"]);
        trace.mode = TraceMode(namedIndex(modes, paint["mode"].text()));
        trace.available = true;
    }
    return analysis;
}

void paintAnalysis(Canvas& canvas, Rect area, const Analysis& analysis) {
    if (area.width <= 20 || area.height <= 12)
        return;
    canvas.save();
    canvas.clip(area);
    canvas.fill(area, analysis.background);
    const auto drawGrid = [&](bool vertical, unsigned majorCount) {
        const auto count = majorCount * analysis.subdivisions;
        for (unsigned i = 0; i <= count; ++i) {
            const auto ratio = double(i) / count;
            const auto shade = i % analysis.subdivisions == 0 ? analysis.major : analysis.minor;
            if (vertical) {
                const auto x = area.x + ratio * area.width;
                canvas.line({x, area.y}, {x, area.y + area.height}, shade);
            } else {
                const auto y = area.y + ratio * area.height;
                canvas.line({area.x, y}, {area.x + area.width, y}, shade);
            }
        }
    };
    drawGrid(true, analysis.columns);
    drawGrid(false, analysis.rows);
    for (unsigned i = 0; i < analysis.rows; ++i) {
        const auto ratio = double(i) / analysis.rows;
        char label[128];
        const auto value = std::lerp(analysis.maximum, analysis.minimum, ratio);
        std::snprintf(label, sizeof label, "%.*f%s%s", int(analysis.precision), value,
                      analysis.unit.empty() ? "" : " ", analysis.unit.c_str());
        canvas.text({area.x + 5, area.y + ratio * area.height + 10}, label, 9, analysis.labels);
    }
    std::array<Point, maxTracePoints> points;
    for (const auto& trace : analysis.traces) {
        const auto count = trace.points.size();
        if (!count)
            continue;
        for (std::size_t i = 0; i < count; ++i)
            points[i] = {area.x + trace.points[i].x * area.width,
                         area.y + (1 - trace.points[i].y) * area.height};
        if (trace.mode == TraceMode::points || count == 1) {
            for (std::size_t i = 0; i < count; ++i)
                canvas.circle(points[i], trace.width, trace.color);
        } else {
            const bool fill = trace.mode == TraceMode::fill;
            canvas.polyline({points.data(), count}, trace.color, trace.width, fill, fill);
        }
    }
    canvas.restore();
}

using Clock = std::chrono::steady_clock;
enum class Easing { linear, ease, easeIn, easeOut, easeInOut };
struct LayerMix {
    struct Item {
        unsigned material{};
        double weight{};
    };
    // A role can use its base and one distinct material from each state.
    std::array<Item, stateNames.size() + 1> values{};
    unsigned count{};
    void add(std::optional<unsigned> material, double weight) noexcept {
        if (!material || weight < 1.e-5)
            return;
        for (unsigned i = 0; i < count; ++i) {
            if (values[i].material == *material) {
                values[i].weight += weight;
                return;
            }
        }
        if (count < values.size())
            values[count++] = {*material, weight};
    }
};

double ease(Easing easing, double t) noexcept {
    constexpr std::array controlPoints{std::array{0., 0., 1., 1.}, std::array{.25, .1, .25, 1.},
                                       std::array{.42, 0., 1., 1.}, std::array{0., 0., .58, 1.},
                                       std::array{.42, 0., .58, 1.}};
    if (easing == Easing::linear || t <= 0 || t >= 1)
        return t;
    const auto [x1, y1, x2, y2] = controlPoints[unsigned(easing)];
    const auto cubic = [](double u, double a, double b) {
        const auto v = 1 - u;
        return 3 * v * v * u * a + 3 * v * u * u * b + u * u * u;
    };
    // Invert the monotone time coordinate. Work happens only while
    // a transition is active, once per painted control.
    double low = 0, high = 1;
    for (unsigned i = 0; i < 12; ++i) {
        const auto midpoint = (low + high) / 2;
        if (cubic(midpoint, x1, x2) < t)
            low = midpoint;
        else
            high = midpoint;
    }
    return cubic((low + high) / 2, y1, y2);
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
    ControlGeometry geometry;
    std::vector<StackLayer> stack;
    std::optional<Analysis> analysis;
    double durationMs{};
    Easing easing = Easing::linear;
    Clock::time_point began{};
    std::array<LayerMix, layerNames.size()> initial;
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

Layers controlLayers(const Control& control) {
    auto layers = control.layers;
    for (unsigned i = 0; i < control.states.size(); ++i)
        if (control.states[i])
            applyLayers(layers, control.variations[i]);
    return layers;
}

double progress(const Control& control, Clock::time_point now) {
    if (control.durationMs <= 0 || control.began == Clock::time_point{})
        return 1.;
    const auto elapsed = std::chrono::duration<double, std::milli>(now - control.began).count();
    return ease(control.easing, std::clamp(elapsed / control.durationMs, 0., 1.));
}

LayerMix blended(const Control& control, unsigned role, std::optional<unsigned> material,
                 double t) {
    LayerMix mix;
    if (t < 1)
        for (unsigned i = 0; i < control.initial[role].count; ++i) {
            const auto& item = control.initial[role].values[i];
            mix.add(item.material, item.weight * (1 - t));
        }
    mix.add(material, t);
    return mix;
}

void shape(Canvas& canvas, Rect rect, const Material& material, Color fill, Color end) {
    const std::array stops{GradientStop{0, fill}, GradientStop{1, end}};
    const auto kind = fill.argb == end.argb ? GradientKind::solid : GradientKind::linear;
    canvas.fillGeometry(rect, stops, fillGeometry(rect, kind, 180), material.corners,
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
    std::size_t traceCapacity{};
    bool reducedMotion{};

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
            if (!value["vector"]["viewBox"].isNull()) {
                const auto box = numbers<4>(value["vector"]["viewBox"], -4096, 4096);
                if (box[2] <= 0 || box[3] <= 0)
                    invalid("invalid vector view box");
                m.viewBox = {box[0], box[1], box[2], box[3]};
            }
            if (!value["vector"]["offset"].isNull()) {
                const auto offset = numbers<2>(value["vector"]["offset"], -256, 256);
                m.iconOffset = {offset[0], offset[1]};
            }
        }
        m.shadow = effect(value["shadow"], EffectKind::shadow);
        m.innerShadow = effect(value["innerShadow"], EffectKind::innerShadow);
        m.glow = effect(value["glow"], EffectKind::shadow);
        m.corners = value["corners"].isNull() ? std::array{m.radius, m.radius, m.radius, m.radius}
                                              : numbers<4>(value["corners"], 0, 48);
        if (!value["gradient"].isNull())
            m.gradient = readGradient(value["gradient"]);
        if (!value["borders"].isNull()) {
            for (const auto& border : array(value["borders"], 4))
                m.borders.push_back({color(border["color"]), number(border["width"], 0, 16)});
        }
        if (const auto& texture = value["texture"]; !texture.isNull()) {
            constexpr std::array fits{"stretch", "contain", "cover", "tile"};
            constexpr std::array blends{"normal", "multiply", "screen"};
            const auto offset = numbers<2>(texture["offset"], -512, 512);
            m.texture = Texture{index(texture["asset"], assets.size()),
                                TextureFit(namedIndex(fits, texture["fit"].text())),
                                PaintBlend(namedIndex(blends, texture["blend"].text())),
                                number(texture["scale"], .125, 8),
                                number(texture["opacity"], 0, 1),
                                {offset[0], offset[1]}};
        }
        if (const auto& slices = value["slices"]; !slices.isNull()) {
            if (!m.asset)
                invalid("slices require an asset");
            constexpr std::array modes{"stretch", "tile"};
            const auto minimum = numbers<2>(slices["minimum"], 0, 4096);
            const auto logical = numbers<4>(slices["insets"], 0, 128);
            const auto source =
                slices["source"].isNull() ? logical : numbers<4>(slices["source"], 0, 128);
            const auto* transparent = slices["transparentCenter"].boolean();
            m.slices = ArtworkSlices{source,
                                     logical,
                                     SliceMode(namedIndex(modes, slices["edges"].text())),
                                     SliceMode(namedIndex(modes, slices["center"].text())),
                                     transparent && *transparent,
                                     {minimum[0], minimum[1]}};
        }
        if (const auto& bevel = value["bevel"]; !bevel.isNull()) {
            const auto kind = bevel["kind"].text();
            if (kind != "raised" && kind != "recessed")
                invalid("unknown bevel geometry");
            auto light = color(bevel["light"]), dark = color(bevel["dark"]);
            if (kind == "recessed")
                std::swap(light, dark);
            m.bevel = Bevel{number(bevel["width"], 0, 12), number(bevel["angle"], -360, 360), light,
                            dark};
        }
        if (const auto& lighting = value["lighting"]; !lighting.isNull()) {
            m.ambient = number(lighting["ambient"], -1, 1);
            m.lightDirection = number(lighting["direction"], -360, 360);
            m.lightIntensity = number(lighting["intensity"], 0, 1);
        }
        if (!value["finish"].isNull()) {
            constexpr std::array names{"matte", "satin", "polished"};
            constexpr std::array sheen{0., .07, .20};
            m.sheen = sheen[namedIndex(names, value["finish"].text())];
        }
        return m;
    }

    StackLayer readStackLayer(const Json& value) const {
        const auto rect = numbers<4>(value["rect"], -2, 3);
        if (rect[2] <= 0 || rect[3] <= 0)
            invalid("invalid layer size");
        const auto anchor = numbers<2>(value["anchor"], 0, 1);
        const auto pivot = numbers<2>(value["pivot"], 0, 1);
        const auto transform = numbers<5>(value["transform"], -360, 360);
        const auto visible = numbers<2>(value["visible"], 0, 1);
        if (visible[1] < visible[0])
            invalid("invalid layer visibility range");
        StackLayer layer;
        layer.material = index(value["material"], materials.size());
        layer.rect = {rect[0], rect[1], rect[2], rect[3]};
        layer.anchor = {anchor[0], anchor[1]};
        layer.pivot = {pivot[0], pivot[1]};
        layer.offset = {transform[0], transform[1]};
        layer.angle = transform[2];
        layer.scale = {transform[3], transform[4]};
        layer.opacity = number(value["opacity"], 0, 1);
        layer.firstVisible = visible[0];
        layer.lastVisible = visible[1];
        const auto* clip = value["clip"].boolean();
        layer.clip = clip && *clip;
        if (!value["state"].isNull())
            layer.state = namedIndex(stateNames, value["state"].text());
        layer.text = value["text"].text();
        if (layer.text.size() > 1024)
            invalid("layer text too long");
        if (const auto& motion = value["motion"]; !motion.isNull()) {
            constexpr std::array targets{"rotate", "x", "y", "fill-x", "fill-y", "opacity"};
            constexpr std::array sources{"value", "indicator"};
            layer.motion = LayerMotion(namedIndex(targets, motion["target"].text()));
            layer.fromIndicator = namedIndex(sources, motion["source"].text()) != 0;
            const auto range = numbers<2>(motion["range"], -360, 360);
            layer.startMotion = range[0];
            layer.endMotion = range[1];
        }
        if (const auto& frame = value["frame"]; !frame.isNull()) {
            constexpr std::array orientations{"vertical", "horizontal"};
            layer.frames = index(frame["count"], 129);
            layer.horizontalFrames = namedIndex(orientations, frame["orientation"].text()) != 0;
            const auto asset = materials[layer.material].asset;
            if (!asset || layer.frames < 2 || !layer.text.empty())
                invalid("invalid filmstrip");
            const auto& surface = *assets[*asset];
            const auto extent = layer.horizontalFrames ? surface.width() : surface.height();
            if (extent < int(layer.frames))
                invalid("filmstrip must provide at least one source pixel per frame");
        }
        return layer;
    }

    ControlGeometry readControlGeometry(const Json& value, Kind kind) const {
        ControlGeometry geometry;
        geometry.vertical = kind == Kind::fader;
        if (value.isNull())
            return geometry;
        geometry.origin = number(value["origin"], 0, 1);
        geometry.ticks = index(value["ticks"], 65);
        if (kind == Kind::knob) {
            constexpr std::array arcs{"none", "unipolar", "bipolar"};
            constexpr std::array caps{"stationary", "rotating"};
            const auto angles = numbers<2>(value["angles"], -360, 360);
            const auto pivot = numbers<2>(value["pivot"], 0, 1);
            if (!(angles[1] > angles[0] && angles[1] - angles[0] <= 360))
                invalid("invalid rotary sweep");
            geometry.firstAngle = angles[0];
            geometry.lastAngle = angles[1];
            geometry.pivot = {pivot[0], pivot[1]};
            geometry.rotateCap = namedIndex(caps, value["cap"].text()) != 0;
            geometry.arc = ControlGeometry::Arc(namedIndex(arcs, value["arc"].text()));
        } else {
            constexpr std::array orientations{"horizontal", "vertical"};
            geometry.vertical = namedIndex(orientations, value["orientation"].text()) != 0;
            if (!value["unity"].isNull())
                geometry.unity = number(value["unity"], 0, 1);
        }
        return geometry;
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
        c.geometry = readControlGeometry(value["geometry"], c.kind);
        if (const auto& transition = value["transition"]; !transition.isNull()) {
            constexpr std::array easings{"linear", "ease", "ease-in", "ease-out", "ease-in-out"};
            c.durationMs = number(transition["duration"], 0, 1500);
            c.easing = Easing(namedIndex(easings, transition["easing"].text()));
        }
        if (!value["analysis"].isNull())
            c.analysis = readAnalysis(value["analysis"]);
        if (!value["stack"].isNull())
            for (const auto& layer : array(value["stack"], 24))
                c.stack.push_back(readStackLayer(layer));
        for (const auto& [name, variation] : object(value["states"], stateNames.size()))
            c.variations[namedIndex(stateNames, name)] = readLayers(variation, materials.size());
        c.states[unsigned(State::enabled)] = true;
        if (const auto* selected = value["selected"].boolean())
            c.states[unsigned(State::selected)] = *selected;
        if (const auto* disabled = value["disabled"].boolean())
            c.states[unsigned(State::disabled)] = *disabled;
        if (!value["activeStates"].isNull()) {
            for (const auto& state : array(value["activeStates"], stateNames.size()))
                c.states[namedIndex(stateNames, state.text())] = true;
        }
        return c;
    }

    void paintTexture(Canvas& canvas, Rect rect, const Texture& texture) const {
        if (texture.opacity <= 0)
            return;
        const auto& image = *assets[texture.asset];
        const double horizontal = rect.width / image.width(),
                     vertical = rect.height / image.height();
        double sx = horizontal, sy = vertical;
        switch (texture.fit) {
        case TextureFit::stretch:
            break;
        case TextureFit::contain:
            sx = sy = std::min(horizontal, vertical);
            break;
        case TextureFit::cover:
            sx = sy = std::max(horizontal, vertical);
            break;
        case TextureFit::tile:
            sx = sy = 1;
            break;
        }
        const double width = image.width() * sx * texture.scale,
                     height = image.height() * sy * texture.scale;
        const Point origin{rect.x + (rect.width - width) / 2 + texture.offset.x,
                           rect.y + (rect.height - height) / 2 + texture.offset.y};
        canvas.save();
        canvas.clip(rect);
        canvas.opacity(texture.opacity);
        canvas.blend(texture.blend);
        if (texture.fit == TextureFit::tile)
            canvas.tileImage(image, rect, {0, 0, double(image.width()), double(image.height())},
                             {width, height}, origin);
        else
            canvas.image(image, {origin.x, origin.y, width, height});
        canvas.restore();
    }

    void faceContent(Canvas& canvas, Rect rect, const Material& m, double scale) const {
        if (!m.path.empty()) {
            if (!canvas.svgPath(m.path, rect, m.fill, m.stroke, m.viewBox, m.iconOffset))
                invalid("unsupported vector path");
            return;
        }
        if (m.gradient) {
            const auto& gradient = *m.gradient;
            canvas.fillGeometry(rect, gradient.stops,
                                fillGeometry(rect, gradient.type, gradient.angle, gradient.center),
                                m.corners, m.ellipse);
        } else {
            shape(canvas, rect, m, m.fill, m.end);
        }
        if (m.asset) {
            const auto& image = *assets[*m.asset];
            if (m.slices) {
                drawSlicedArtwork(canvas, image, rect, *m.slices, scale);
            } else {
                const double ratio =
                    std::min(rect.width / image.width(), rect.height / image.height());
                const double width = image.width() * ratio, height = image.height() * ratio;
                canvas.image(image, {rect.x + (rect.width - width) / 2,
                                     rect.y + (rect.height - height) / 2, width, height});
            }
        }
        if (m.texture)
            paintTexture(canvas, rect, *m.texture);
        constexpr Color white(0xffffffff), black(0xff000000), transparent(0);
        if (m.ambient != 0) {
            const auto tint = tintAlpha(m.ambient > 0 ? white : black, std::abs(m.ambient));
            shape(canvas, rect, m, tint, tint);
        }
        const double light = std::min(1., m.lightIntensity + m.sheen);
        if (light > 0) {
            const std::array stops{GradientStop{0, tintAlpha(white, light * .30)},
                                   GradientStop{.52, transparent},
                                   GradientStop{1, tintAlpha(black, light * .25)}};
            canvas.fillGeometry(rect, stops,
                                fillGeometry(rect, GradientKind::linear, m.lightDirection),
                                m.corners, m.ellipse);
        }
        const auto outline = [&](Border line, double inset) {
            if (line.width <= 0)
                return;
            const auto inner = rect.inset(inset + line.width / 2);
            if (inner.width <= 0 || inner.height <= 0)
                return;
            if (m.ellipse) {
                canvas.ellipse(inner, line.color, line.width);
            } else {
                auto corners = m.corners;
                for (auto& radius : corners)
                    radius = std::max(0., radius - inset - line.width / 2);
                canvas.strokeCorners(inner, corners, line.color, line.width);
            }
        };
        double inset = 0;
        if (m.borders.empty()) {
            outline({m.edge, m.edgeWidth}, 0);
            inset = m.edgeWidth;
        }
        for (const auto& border : m.borders) {
            outline(border, inset);
            inset += border.width;
        }
        if (m.bevel && m.bevel->width > 0) {
            const auto& bevel = *m.bevel;
            const auto inner = rect.inset(inset + bevel.width / 2);
            const std::array stops{GradientStop{0, bevel.light}, GradientStop{.5, transparent},
                                   GradientStop{1, bevel.dark}};
            auto corners = m.corners;
            for (auto& radius : corners)
                radius = std::max(0., radius - inset - bevel.width / 2);
            canvas.fillGeometry(inner, stops,
                                fillGeometry(inner, GradientKind::linear, bevel.angle), corners,
                                m.ellipse, bevel.width);
        }
        if (m.highlight.argb >> 24 && rect.width > 3 && rect.height > 3) {
            if (m.ellipse) {
                canvas.ellipse(rect.inset(1), m.highlight, 1);
            } else {
                canvas.line({rect.x + std::max(1., m.corners[0] / 2), rect.y + 1},
                            {rect.x + rect.width - std::max(1., m.corners[1] / 2), rect.y + 1},
                            m.highlight);
            }
        }
    }

    void buildMaterial(Canvas& canvas, Rect rect, const Material& m, double scale) {
        constexpr Color white(0xffffffff);
        const int width = int(std::ceil(rect.width * scale));
        const int height = int(std::ceil(rect.height * scale));
        AlphaMask mask;
        if (m.shadow || m.innerShadow || m.glow || m.mask || m.asset || m.texture) {
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
        if (m.mask || m.asset || m.texture) {
            Surface content;
            content.setPixels(std::vector<std::uint32_t>(std::size_t(width) * height), width,
                              height);
            content.render([&](Canvas& offscreen) {
                offscreen.scale(scale, scale);
                faceContent(offscreen, {0, 0, rect.width, rect.height}, m, scale);
                if (m.innerShadow)
                    effects.draw(offscreen, mask, {0, 0}, *m.innerShadow, scale);
            });
            maskPixels(content, mask.pixels());
            canvas.image(content, {rect.x, rect.y, width / scale, height / scale});
        } else {
            faceContent(canvas, rect, m, scale);
            if (m.innerShadow)
                effects.draw(canvas, mask, {rect.x, rect.y}, *m.innerShadow, scale);
        }
    }

    void drawLayer(Canvas& canvas, std::optional<unsigned> which, Rect rect, double scale,
                   double opacity = 1) {
        if (!which || rect.width <= 0 || rect.height <= 0)
            return;
        const auto& m = materials[*which];
        if (m.opacity == 0)
            return;
        // Cache work with a real preparation cost. Retaining a full-panel
        // image for a simple fill would evict small, expensive blurred layers
        // at high DPI and force the entire sequence to rebuild next frame.
        if (!m.shadow && !m.innerShadow && !m.glow && !m.mask && !m.asset && !m.texture &&
            m.path.empty()) {
            canvas.save();
            canvas.opacity(m.opacity * opacity);
            faceContent(canvas, rect, m, scale);
            canvas.restore();
            return;
        }
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
        canvas.opacity(m.opacity * opacity);
        if (cost > layerBudget) {
            // A background larger than the material budget is still drawable.
            // Cap its intermediate raster work and cache it at a lower DPI.
            const auto reduced = std::min(
                scale,
                std::floor(std::sqrt(layerBudget / (4 * padded.width * padded.height)) * 4) / 4);
            canvas.restore();
            if (reduced >= .25 && reduced < scale)
                drawLayer(canvas, which, rect, reduced, opacity);
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

    void paintStack(Canvas& canvas, const Control& control, double scale) {
        for (const auto& layer : control.stack) {
            if (layer.state && !control.states[*layer.state])
                continue;
            const auto value = layer.fromIndicator ? control.indicatorValue : control.value;
            if (value < layer.firstVisible || value > layer.lastVisible)
                continue;
            const Rect box = control.bounds;
            const Rect area{box.x + (layer.rect.x + layer.anchor.x) * box.width,
                            box.y + (layer.rect.y + layer.anchor.y) * box.height,
                            layer.rect.width * box.width, layer.rect.height * box.height};
            Point offset = layer.offset;
            Point stretch = layer.scale;
            double angle = layer.angle, opacity = layer.opacity;
            if (layer.motion) {
                const double amount = std::lerp(layer.startMotion, layer.endMotion, value);
                switch (*layer.motion) {
                case LayerMotion::rotate:
                    angle += amount;
                    break;
                case LayerMotion::x:
                    offset.x += amount;
                    break;
                case LayerMotion::y:
                    offset.y += amount;
                    break;
                case LayerMotion::fillX:
                    stretch.x *= std::clamp(amount, 0., 1.);
                    break;
                case LayerMotion::fillY:
                    stretch.y *= std::clamp(amount, 0., 1.);
                    break;
                case LayerMotion::opacity:
                    opacity *= std::clamp(amount, 0., 1.);
                    break;
                }
            }
            if (opacity <= 0 || stretch.x == 0 || stretch.y == 0)
                continue;
            canvas.save();
            if (layer.clip)
                canvas.clip(box);
            canvas.translate(offset.x * area.width, offset.y * area.height);
            const Point pivot{area.x + area.width * layer.pivot.x,
                              area.y + area.height * layer.pivot.y};
            canvas.translate(pivot.x, pivot.y);
            canvas.rotate(angle * std::numbers::pi / 180);
            canvas.scale(stretch.x, stretch.y);
            canvas.translate(-pivot.x, -pivot.y);
            if (layer.frames) {
                const auto& asset = *assets[*materials[layer.material].asset];
                const auto frame = std::round(value * (layer.frames - 1));
                const auto extent = double(layer.horizontalFrames ? asset.width() : asset.height());
                const auto first = std::round(frame * extent / layer.frames);
                const auto last = std::round((frame + 1) * extent / layer.frames);
                const Rect crop = layer.horizontalFrames
                                      ? Rect{first, 0, last - first, double(asset.height())}
                                      : Rect{0, first, double(asset.width()), last - first};
                canvas.opacity(opacity);
                canvas.image(asset, area, crop, false);
            } else if (!layer.text.empty()) {
                canvas.opacity(opacity);
                canvas.fittedText(area, layer.text, materials[layer.material].fill, control.weight,
                                  0, control.typeface);
            } else {
                drawLayer(canvas, layer.material, area, scale, opacity);
            }
            canvas.restore();
        }
    }

    static Point anglePoint(Point center, double radius, double angle) {
        const auto radians = (angle - 90) * std::numbers::pi / 180;
        return {center.x + std::cos(radians) * radius, center.y + std::sin(radians) * radius};
    }

    static void paintArc(Canvas& canvas, Point center, double radius, double first, double last,
                         Color color) {
        if (radius < 4 || first == last)
            return;
        std::array<Point, 32> arc;
        for (unsigned i = 0; i < arc.size(); ++i)
            arc[i] =
                anglePoint(center, radius, std::lerp(first, last, double(i) / (arc.size() - 1)));
        canvas.polyline(arc, color, 2);
    }

    void paintControl(Canvas& canvas, const Control& control, double scale, Clock::time_point now) {
        if (control.rotation) {
            canvas.save();
            const auto& r = control.bounds;
            const auto x = r.x + r.width / 2, y = r.y + r.height / 2;
            canvas.translate(x, y);
            canvas.rotate(control.rotation * std::numbers::pi / 180);
            canvas.translate(-x, -y);
        }
        const auto layersNow = controlLayers(control);
        const double transition = reducedMotion ? 1. : progress(control, now);
        const auto rect = control.bounds;
        const auto draw = [&](Layer layer, Rect bounds) {
            if (transition == 1) {
                drawLayer(canvas, layersNow[layer], bounds, scale);
                return;
            }
            const auto mix = blended(control, layer, layersNow[layer], transition);
            for (unsigned i = 0; i < mix.count; ++i) {
                const auto& paint = mix.values[i];
                drawLayer(canvas, paint.material, bounds, scale, paint.weight);
            }
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
            const auto& g = control.geometry;
            const auto angle = std::lerp(g.firstAngle, g.lastAngle, control.value);
            const Point center{centerX, centerY};
            const Point pivot{dial.x + dial.width * g.pivot.x, dial.y + dial.height * g.pivot.y};
            draw(track, dial.inset(-4));
            if (!g.rotateCap) {
                draw(face, dial);
                draw(highlight, dial.inset(diameter * .08));
            }
            canvas.save();
            canvas.translate(pivot.x, pivot.y);
            canvas.rotate(angle * std::numbers::pi / 180);
            canvas.translate(-pivot.x, -pivot.y);
            if (g.rotateCap) {
                draw(face, dial);
                draw(highlight, dial.inset(diameter * .08));
            }
            draw(pointer, {centerX - 2, centerY - diameter * .39, 4, diameter * .20});
            canvas.restore();
            if (diameter >= 20) {
                for (unsigned i = 0; i < g.ticks; ++i) {
                    const auto tick =
                        std::lerp(g.firstAngle, g.lastAngle, double(i) / std::max(1u, g.ticks - 1));
                    canvas.line(anglePoint(center, diameter / 2 + 4, tick),
                                anglePoint(center, diameter / 2 + 8, tick), textColor);
                }
                if (layersNow[indicator]) {
                    const auto color = materials[*layersNow[indicator]].fill;
                    if (g.arc != ControlGeometry::Arc::none) {
                        const auto origin = g.arc == ControlGeometry::Arc::bipolar ? g.origin : 0.;
                        paintArc(canvas, center, diameter / 2 + 5,
                                 std::lerp(g.firstAngle, g.lastAngle, origin), angle, color);
                    }
                    if (control.indicatorValue > 0)
                        paintArc(canvas, center,
                                 diameter / 2 + (g.arc == ControlGeometry::Arc::none ? 6 : 10),
                                 g.firstAngle,
                                 std::lerp(g.firstAngle, g.lastAngle, control.indicatorValue),
                                 color);
                }
            }
            break;
        }
        case Kind::fader:
        case Kind::slider:
        case Kind::meter: {
            const auto& g = control.geometry;
            const double low = std::min(control.value, g.origin),
                         high = std::max(control.value, g.origin);
            const Rect rail =
                g.vertical ? Rect{centerX - 6, content.y + 4, 12, std::max(0., content.height - 8)}
                           : Rect{content.x, centerY - 7, content.width, 14};
            draw(track, rail);
            canvas.save();
            canvas.clip(g.vertical ? Rect{rail.x, rail.y + rail.height * (1 - high), rail.width,
                                          rail.height * (high - low)}
                                   : Rect{rail.x + rail.width * low, rail.y,
                                          rail.width * (high - low), rail.height});
            draw(indicator, rail);
            canvas.restore();
            const auto drawMark = [&](double position, double length) {
                if (g.vertical) {
                    const double y = rail.y + rail.height * (1 - position);
                    canvas.line({rail.x + rail.width, y}, {rail.x + rail.width + length, y},
                                textColor);
                } else {
                    const double x = rail.x + rail.width * position;
                    canvas.line({x, rail.y + rail.height}, {x, rail.y + rail.height + length},
                                textColor);
                }
            };
            for (unsigned i = 0; i < g.ticks; ++i)
                drawMark(double(i) / std::max(1u, g.ticks - 1), 3);
            if (g.unity)
                drawMark(*g.unity, 6);
            if (control.kind != Kind::meter) {
                const bool fader = control.kind == Kind::fader;
                const double w = fader ? 42 : 14, h = fader ? 16 : 18;
                const double x = g.vertical ? centerX : rail.x + rail.width * control.value;
                const double y = g.vertical ? rail.y + rail.height * (1 - control.value) : centerY;
                const Rect cap{x - w / 2, y - h / 2, w, h};
                draw(face, cap);
                if (fader)
                    draw(pointer, {cap.x + 5, cap.y + 7, 32, 2});
            }
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
        if (control.analysis)
            paintAnalysis(canvas, content, *control.analysis);
        draw(foreground, rect);
        paintStack(canvas, control, scale);
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
    auto& control = *impl_->byId[id];
    if (control.states[unsigned(state)] == active)
        return true;
    if (impl_->reducedMotion || control.durationMs == 0) {
        control.states[unsigned(state)] = active;
        return true;
    }
    const auto before = controlLayers(control);
    control.states[unsigned(state)] = active;
    if (before == controlLayers(control))
        return true;
    const auto now = Clock::now();
    const auto t = progress(control, now);
    for (unsigned role = 0; role < before.size(); ++role)
        control.initial[role] = blended(control, role, before[role], t);
    control.began = now;
    return true;
}

void Scene::setReducedMotion(bool reduced) noexcept {
    impl_->reducedMotion = reduced;
    if (reduced)
        for (auto& control : impl_->controls)
            control.began = {};
}

bool Scene::needsFrame() const noexcept {
    if (impl_->reducedMotion)
        return false;
    const auto now = Clock::now();
    return std::any_of(impl_->controls.begin(), impl_->controls.end(),
                       [&](const auto& control) { return progress(control, now) < 1; });
}

bool Scene::setTrace(unsigned id, TraceRole role, std::span<const Point> points) {
    if (!controlBounds(id) || unsigned(role) >= analysisNames.size() ||
        points.size() > maxTracePoints)
        return false;
    auto& view = impl_->byId[id]->analysis;
    if (!view)
        return false;
    auto& trace = view->traces[unsigned(role)];
    if (!trace.available || std::any_of(points.begin(), points.end(), [](Point point) {
            return !(point.x >= 0 && point.x <= 1 && point.y >= 0 && point.y <= 1);
        }))
        return false;
    if (points.size() > trace.points.capacity()) {
        if (impl_->traceCapacity - trace.points.capacity() + points.size() > 32768)
            return false;
        std::vector<Point> replacement(points.begin(), points.end());
        impl_->traceCapacity += replacement.capacity() - trace.points.capacity();
        trace.points.swap(replacement);
    } else {
        trace.points.assign(points.begin(), points.end());
    }
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
    const auto now = Clock::now();
    for (const auto& control : impl_->controls)
        impl_->paintControl(canvas, control, scale, now);
}

CacheStats Scene::cacheStats() const noexcept {
    return {impl_->layers.size(), impl_->bytes, impl_->builds, impl_->effects.stats().blurBuilds};
}

} // namespace cell::ui::skins
