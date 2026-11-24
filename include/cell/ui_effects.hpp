#pragma once

#include <cell/ui.hpp>
#include <cell/graphics_effects.hpp>

namespace cell::ui {
// GUI-thread-owned alpha geometry. Identity survives repeated paints, revision
// advances only when pixels change. No cache borrows its storage or lifetime.
class AlphaMask {
public:
    AlphaMask();
    AlphaMask(const AlphaMask&) = delete;
    AlphaMask& operator=(const AlphaMask&) = delete;
    void setPixels(std::span<const std::uint8_t>, int width, int height);
    void capture(const Surface&);
    void rasterize(int width, int height, const std::function<void(Canvas&)>&);
    std::span<const std::uint8_t> pixels() const noexcept { return pixels_; }
    int width() const noexcept { return width_; }
    int height() const noexcept { return height_; }
    std::uint64_t identity() const noexcept { return identity_; }
    std::uint64_t revision() const noexcept { return revision_; }
private:
    const std::uint64_t identity_;
    std::uint64_t revision_{};
    int width_{}, height_{};
    std::vector<std::uint8_t> pixels_;
};
enum class EffectKind { shadow, innerShadow };
struct ShadowStyle {
    double sigma = 6;       // logical pixels; mask itself is already rasterized at scale
    Point offset{0, 3};
    Color color{0x80000000}; // straight alpha tint, premultiplied internally
    double opacity = 1;
    EffectKind kind = EffectKind::shadow;
};
struct EffectCacheLimits { std::size_t bytes = 16u*1024*1024, entries = 64; };
struct EffectCacheStats {
    std::size_t entries{}, retainedBytes{}, scratchBytes{};
    std::uint64_t blurBuilds{}, hits{}, colorBuilds{}, evictions{}, uncachedDraws{};
};
class EffectCache {
public:
    explicit EffectCache(EffectCacheLimits = {});
    ~EffectCache();
    EffectCache(const EffectCache&) = delete;
    EffectCache& operator=(const EffectCache&) = delete;
    // Per-editor GUI-thread cache. Color/offset/opacity changes never reblur.
    // Zero budget renders uncached; work still obeys graphics image limits.
    void draw(Canvas&, const AlphaMask&, Point logicalOrigin, const ShadowStyle&, double scale = 1);
    void setLimits(EffectCacheLimits);
    void clear();
    void releaseScratch();
    EffectCacheStats stats() const noexcept;
private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

// Reusable transparent vector/text layer. Content version, logical size and DPI
// control repainting; screen position does not. Call invalidate for local edits.
// Paint callback receives logical coordinates and must not retain Canvas.
class CachedLayer {
public:
    CachedLayer() = default;
    CachedLayer(const CachedLayer&) = delete;
    CachedLayer& operator=(const CachedLayer&) = delete;
    CachedLayer(CachedLayer&&) noexcept;
    CachedLayer& operator=(CachedLayer&&) noexcept;
    void draw(Canvas&, Rect destination, double scale, std::uint64_t contentVersion,
              const std::function<void(Canvas&)>&);
    void invalidate() noexcept { valid_ = false; }
    void clear() noexcept;
    std::uint64_t paintCount() const noexcept { return paints_; }
    std::size_t retainedBytes() const noexcept;
private:
    std::unique_ptr<Surface> surface_;
    double width_{}, height_{}, scale_{};
    std::uint64_t version_{}, paints_{};
    bool valid_{};
};
} // namespace cell::ui
