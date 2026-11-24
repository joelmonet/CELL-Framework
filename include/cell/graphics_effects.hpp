#pragma once

#include <cstddef>
#include <cstdint>
#include <span>
#include <vector>

namespace cell::graphics {
// Independently implemented CPU effects. Worker/GUI use only; these functions
// allocate scratch storage and are not audio-callback services.
enum class EdgeMode { transparent, clamp };
inline constexpr std::size_t maximumPixels = 16u * 1024 * 1024;
inline constexpr int maximumEdge = 8192;
inline constexpr double maximumSigma = 256;
std::size_t checkedPixelCount(int width, int height);
int blurPadding(double sigma);

class BlurWorkspace {
public:
    // sigma is the approximate Gaussian standard deviation, in physical pixels.
    // Three continuous fractional boxes avoid radius steps. Input rows are tight.
    // Validation/allocation failures leave the input unchanged. ARGB must already
    // be premultiplied; the positive filter preserves that relationship.
    void alpha(std::span<std::uint8_t>, int width, int height, double sigma,
               EdgeMode = EdgeMode::transparent);
    void argb(std::span<std::uint32_t>, int width, int height, double sigma,
              EdgeMode = EdgeMode::transparent);
    std::size_t retainedBytes() const noexcept;
    void release() noexcept;
private:
    std::vector<float> first_, second_;
    void prepare(std::size_t);
    void filter(int width, int height, double sigma, EdgeMode) noexcept;
};
} // namespace cell::graphics
