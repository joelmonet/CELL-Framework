#include <cell/ui_effects.hpp>
#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstring>
#include <list>
#include <stdexcept>

namespace cell::ui {
namespace {
std::atomic<std::uint64_t> nextMask{1};
void finitePoint(Point p) {
    if (!std::isfinite(p.x) || !std::isfinite(p.y)) throw std::invalid_argument("effect position");
}
void validScale(double scale) {
    if (!std::isfinite(scale) || scale <= 0 || scale > 8) throw std::invalid_argument("graphics scale outside (0,8]");
}
std::unique_ptr<Surface> transparentSurface(int w, int h) {
    auto s = std::make_unique<Surface>();
    s->setPixels(std::vector<std::uint32_t>(graphics::checkedPixelCount(w,h)), w, h);
    return s;
}
std::uint32_t tinted(double alpha, Color color, double opacity) {
    const auto a = unsigned(std::clamp(std::lround(alpha * double(color.argb >> 24) / 255 * opacity), 0l, 255l));
    auto channel = [&](unsigned shift) { return (((color.argb >> shift) & 255u) * a + 127u) / 255u; };
    return (a << 24) | (channel(16) << 16) | (channel(8) << 8) | channel(0);
}
}
AlphaMask::AlphaMask() : identity_(nextMask.fetch_add(1, std::memory_order_relaxed)) {}
void AlphaMask::setPixels(std::span<const std::uint8_t> pixels, int w, int h) {
    if (graphics::checkedPixelCount(w,h) != pixels.size()) throw std::invalid_argument("alpha mask span");
    if (width_ == w && height_ == h && std::equal(pixels.begin(), pixels.end(), pixels_.begin())) return;
    // Stage first: caller may supply a view of this mask's own pixels.
    std::vector<std::uint8_t> next(pixels.begin(), pixels.end());
    pixels_.swap(next); width_ = w; height_ = h; ++revision_;
}
void AlphaMask::capture(const Surface& source) {
    std::vector<std::uint8_t> next(graphics::checkedPixelCount(source.width(), source.height()));
    for (int y = 0; y < source.height(); ++y) {
        const auto* row = static_cast<const std::byte*>(source.pixels()) + y * source.stride();
        for (int x = 0; x < source.width(); ++x) {
            std::uint32_t pixel; std::memcpy(&pixel, row + x * sizeof(pixel), sizeof(pixel));
            next[std::size_t(y) * source.width() + x] = std::uint8_t(pixel >> 24);
        }
    }
    setPixels(next, source.width(), source.height());
}
void AlphaMask::rasterize(int w, int h, const std::function<void(Canvas&)>& paint) {
    auto surface = transparentSurface(w,h);
    surface->render(paint); capture(*surface);
}
struct EffectCache::Impl {
    struct Entry {
        std::uint64_t identity{}, revision{};
        double sigma{};
        int width{}, height{}, pad{};
        std::vector<std::uint8_t> alpha;
        std::unique_ptr<Surface> surface;
        Color color{0}; double opacity{}, dx{}, dy{};
        EffectKind kind{};
        std::size_t bytes() const noexcept {
            return alpha.capacity() + (surface ? std::size_t(surface->stride()) * surface->height() : 0);
        }
    };
    EffectCacheLimits limits;
    std::list<Entry> entries;
    graphics::BlurWorkspace workspace;
    EffectCacheStats counters;
    explicit Impl(EffectCacheLimits l) : limits(l) {}
    std::size_t bytes() const noexcept { std::size_t n = 0; for (const auto& e : entries) n += e.bytes(); return n; }
    void trim() {
        while (!entries.empty() && (entries.size() > limits.entries || bytes() > limits.bytes)) {
            entries.pop_back(); ++counters.evictions;
        }
    }
    Entry build(const AlphaMask& mask, double sigma) {
        Entry e; e.identity = mask.identity(); e.revision = mask.revision(); e.sigma = sigma;
        e.pad = graphics::blurPadding(sigma);
        e.width = mask.width() + 2 * e.pad; e.height = mask.height() + 2 * e.pad;
        e.alpha.resize(graphics::checkedPixelCount(e.width, e.height));
        for (int y = 0; y < mask.height(); ++y)
            std::copy_n(mask.pixels().begin() + std::size_t(y) * mask.width(), mask.width(),
                        e.alpha.begin() + std::size_t(y + e.pad) * e.width + e.pad);
        workspace.alpha(e.alpha, e.width, e.height, sigma); ++counters.blurBuilds;
        return e;
    }
    void colorize(Entry& e, const AlphaMask& mask, const ShadowStyle& style, double scale) {
        const double dx = style.kind == EffectKind::innerShadow ? style.offset.x * scale : 0;
        const double dy = style.kind == EffectKind::innerShadow ? style.offset.y * scale : 0;
        if (e.surface && e.color.argb == style.color.argb && e.opacity == style.opacity &&
            e.kind == style.kind && e.dx == dx && e.dy == dy) return;
        const bool inner = style.kind == EffectKind::innerShadow;
        const int w = inner ? mask.width() : e.width, h = inner ? mask.height() : e.height;
        std::vector<std::uint32_t> pixels(graphics::checkedPixelCount(w,h));
        auto sample = [&](double x, double y) -> double {
            if (x <= -1 || y <= -1 || x >= e.width || y >= e.height) return 0;
            const int ix = int(std::floor(x)), iy = int(std::floor(y));
            auto at = [&](int xx, int yy) -> double {
                return xx < 0 || yy < 0 || xx >= e.width || yy >= e.height ? 0 : e.alpha[std::size_t(yy)*e.width+xx];
            };
            const double fx = x-ix, fy = y-iy;
            return (at(ix,iy)*(1-fx)+at(ix+1,iy)*fx)*(1-fy) + (at(ix,iy+1)*(1-fx)+at(ix+1,iy+1)*fx)*fy;
        };
        for (int y = 0; y < h; ++y) for (int x = 0; x < w; ++x) {
            const auto index = std::size_t(y) * w + x;
            const double a = inner ? mask.pixels()[index] *
                (1 - sample(x + e.pad - dx, y + e.pad - dy) / 255) : e.alpha[index];
            pixels[index] = tinted(a, style.color, style.opacity);
        }
        auto next = std::make_unique<Surface>(); next->setPixels(pixels,w,h);
        e.surface = std::move(next); e.color = style.color; e.opacity = style.opacity;
        e.kind = style.kind; e.dx = dx; e.dy = dy; ++counters.colorBuilds;
    }
};
EffectCache::EffectCache(EffectCacheLimits limits) : impl_(std::make_unique<Impl>(limits)) {}
EffectCache::~EffectCache() = default;
void EffectCache::draw(Canvas& canvas, const AlphaMask& mask, Point origin, const ShadowStyle& style, double scale) {
    validScale(scale); finitePoint(origin); finitePoint(style.offset);
    if (!std::isfinite(style.opacity) || style.opacity < 0 || style.opacity > 1)
        throw std::invalid_argument("shadow opacity outside 0..1");
    const double sigma = style.sigma * scale;
    const int pad = graphics::blurPadding(sigma);
    (void)graphics::checkedPixelCount(mask.width(), mask.height());
    const auto paddedCount = graphics::checkedPixelCount(mask.width()+2*pad, mask.height()+2*pad);
    if (style.opacity == 0 || (style.color.argb >> 24) == 0) return;
    auto& p = *impl_;
    auto found = std::find_if(p.entries.begin(), p.entries.end(), [&](const auto& e) {
        return e.identity == mask.identity() && e.revision == mask.revision() && e.sigma == sigma;
    });
    Impl::Entry temporary;
    Impl::Entry* entry;
    if (found != p.entries.end()) {
        p.entries.splice(p.entries.begin(),p.entries,found); entry = &p.entries.front(); ++p.counters.hits;
    } else if (p.limits.entries && paddedCount <= p.limits.bytes / 5) {
        // Evict before constructing another retained image, not after the frame.
        while (!p.entries.empty() && (p.entries.size() >= p.limits.entries ||
               p.bytes() > p.limits.bytes - paddedCount*5)) {
            p.entries.pop_back(); ++p.counters.evictions;
        }
        p.entries.push_front(p.build(mask,sigma)); entry = &p.entries.front();
    } else {
        temporary = p.build(mask,sigma); entry = &temporary; ++p.counters.uncachedDraws;
    }
    p.colorize(*entry,mask,style,scale);
    const bool inner = style.kind == EffectKind::innerShadow;
    const double x = inner ? origin.x : origin.x + style.offset.x - pad/scale;
    const double y = inner ? origin.y : origin.y + style.offset.y - pad/scale;
    canvas.image(*entry->surface,{x,y,entry->surface->width()/scale,entry->surface->height()/scale});
    p.trim();
}
void EffectCache::setLimits(EffectCacheLimits limits) { impl_->limits = limits; impl_->trim(); }
void EffectCache::clear() { impl_->entries.clear(); }
void EffectCache::releaseScratch() { impl_->workspace.release(); }
EffectCacheStats EffectCache::stats() const noexcept {
    auto s = impl_->counters; s.entries = impl_->entries.size(); s.retainedBytes = impl_->bytes();
    s.scratchBytes = impl_->workspace.retainedBytes(); return s;
}
CachedLayer::CachedLayer(CachedLayer&& other) noexcept { *this = std::move(other); }
CachedLayer& CachedLayer::operator=(CachedLayer&& other) noexcept {
    if (this == &other) return *this;
    surface_ = std::move(other.surface_);
    width_ = other.width_; height_ = other.height_; scale_ = other.scale_;
    version_ = other.version_; paints_ = other.paints_; valid_ = other.valid_;
    other.valid_ = false; other.paints_ = 0;
    return *this;
}
void CachedLayer::draw(Canvas& canvas, Rect dest, double scale, std::uint64_t version,
                       const std::function<void(Canvas&)>& paint) {
    validScale(scale); finitePoint({dest.x,dest.y});
    if (!std::isfinite(dest.width) || !std::isfinite(dest.height) || dest.width <= 0 || dest.height <= 0 ||
        dest.width * scale > graphics::maximumEdge || dest.height * scale > graphics::maximumEdge)
        throw std::invalid_argument("cached layer dimensions");
    const int w = int(std::ceil(dest.width*scale)), h = int(std::ceil(dest.height*scale));
    (void)graphics::checkedPixelCount(w,h);
    if (!valid_ || width_ != dest.width || height_ != dest.height || scale_ != scale || version_ != version) {
        auto next = transparentSurface(w,h);
        next->render([&](Canvas& c) { c.scale(scale,scale); c.clip({0,0,dest.width,dest.height}); paint(c); });
        surface_ = std::move(next); width_ = dest.width; height_ = dest.height;
        scale_ = scale; version_ = version; valid_ = true; ++paints_;
    }
    // Preserve ceil-rounded physical pixels at fractional DPI. Clip instead of
    // stretching those pixels back into a smaller rectangle (which distorts edges).
    canvas.save(); canvas.clip(dest);
    canvas.image(*surface_,{dest.x,dest.y,w/scale,h/scale}); canvas.restore();
}
void CachedLayer::clear() noexcept { surface_.reset(); valid_ = false; }
std::size_t CachedLayer::retainedBytes() const noexcept {
    return surface_ ? std::size_t(surface_->stride()) * surface_->height() : 0;
}
} // namespace cell::ui
