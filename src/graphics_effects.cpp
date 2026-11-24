#include <cell/graphics_effects.hpp>
#include <algorithm>
#include <cmath>
#include <stdexcept>

namespace cell::graphics {
namespace {
struct Box { int radius; double fringe, denominator; };
Box box(double sigma) {
    if (!std::isfinite(sigma) || sigma < 0 || sigma > maximumSigma)
        throw std::invalid_argument("blur sigma outside 0..256 physical pixels");
    const int radius = int(std::floor((std::sqrt(1 + 4 * sigma * sigma) - 1) / 2));
    const double r = radius, variance = sigma * sigma / 3;
    const double base = r * (r + 1) * (2 * r + 1) / 3;
    const double fringe = std::clamp((variance * (2 * r + 1) - base) /
                                    (2 * ((r + 1) * (r + 1) - variance)), 0., 1.);
    return {radius, fringe, 2 * r + 1 + 2 * fringe};
}
// Running sums make the pass cost independent of blur radius. Each line uses
// double accumulators; floating scratch avoids six intermediate 8-bit roundings.
void pass(std::span<const float> source, std::span<float> destination,
          int width, int height, Box kernel, bool vertical, EdgeMode edge) noexcept {
    const int length = vertical ? height : width, lines = vertical ? width : height;
    const std::size_t step = vertical ? std::size_t(width) : 1;
    for (int line = 0; line < lines; ++line) {
        const std::size_t start = vertical ? std::size_t(line) : std::size_t(line) * width;
        auto sample = [&](int index) -> double {
            if (edge == EdgeMode::transparent && (index < 0 || index >= length)) return 0;
            index = std::clamp(index, 0, length - 1);
            return source[start + std::size_t(index) * step];
        };
        double sum = 0;
        for (int i = 0; i <= std::min(kernel.radius, length - 1); ++i) sum += sample(i);
        if (edge == EdgeMode::clamp) {
            sum += kernel.radius * sample(0);
            sum += std::max(0, kernel.radius - length + 1) * sample(length - 1);
        }
        for (int i = 0; i < length; ++i) {
            destination[start + std::size_t(i) * step] = float((sum + kernel.fringe *
                (sample(i - kernel.radius - 1) + sample(i + kernel.radius + 1))) / kernel.denominator);
            sum += sample(i + kernel.radius + 1) - sample(i - kernel.radius);
        }
    }
}
std::uint32_t quantize(float v) noexcept { return std::uint32_t(std::clamp(std::lround(v), 0l, 255l)); }
void validate(std::size_t size, int w, int h, double sigma) {
    if (checkedPixelCount(w, h) != size) throw std::invalid_argument("blur pixel span size");
    (void)box(sigma);
}
}
std::size_t checkedPixelCount(int width, int height) {
    if (width < 1 || height < 1 || width > maximumEdge || height > maximumEdge ||
        std::size_t(width) > maximumPixels / std::size_t(height))
        throw std::invalid_argument("graphics image bounds");
    return std::size_t(width) * std::size_t(height);
}
int blurPadding(double sigma) {
    const auto b = box(sigma);
    return 3 * (b.radius + (b.fringe > 0 ? 1 : 0));
}
void BlurWorkspace::prepare(std::size_t count) { first_.resize(count); second_.resize(count); }
void BlurWorkspace::filter(int width, int height, double sigma, EdgeMode edge) noexcept {
    const auto kernel = box(sigma); // validated before mutation
    for (int repeat = 0; repeat < 3; ++repeat) {
        pass(first_, second_, width, height, kernel, false, edge);
        pass(second_, first_, width, height, kernel, true, edge);
    }
}
void BlurWorkspace::alpha(std::span<std::uint8_t> pixels, int w, int h, double sigma, EdgeMode edge) {
    validate(pixels.size(), w, h, sigma);
    if (sigma == 0) return;
    prepare(pixels.size());
    std::copy(pixels.begin(), pixels.end(), first_.begin());
    filter(w, h, sigma, edge);
    for (std::size_t i = 0; i < pixels.size(); ++i) pixels[i] = std::uint8_t(quantize(first_[i]));
}
void BlurWorkspace::argb(std::span<std::uint32_t> pixels, int w, int h, double sigma, EdgeMode edge) {
    validate(pixels.size(), w, h, sigma);
    if (sigma == 0) return;
    prepare(pixels.size());
    for (unsigned shift : {0u, 8u, 16u, 24u}) {
        for (std::size_t i = 0; i < pixels.size(); ++i) first_[i] = float((pixels[i] >> shift) & 255u);
        filter(w, h, sigma, edge);
        for (std::size_t i = 0; i < pixels.size(); ++i)
            pixels[i] = (pixels[i] & ~(255u << shift)) | (quantize(first_[i]) << shift);
    }
}
std::size_t BlurWorkspace::retainedBytes() const noexcept { return (first_.capacity() + second_.capacity()) * sizeof(float); }
void BlurWorkspace::release() noexcept { std::vector<float>().swap(first_); std::vector<float>().swap(second_); }
} // namespace cell::graphics
