// Original Cell Framework implementation; see LICENSE.
#pragma once
#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstring>

namespace cell::dsp {
// Allocation-free planar kernels. The compiler may vectorize independent
// samples without enabling fast-math or reducing the precision of the signal.
struct Vector {
    static void clear(float* destination, std::size_t count) noexcept {
        if (count) std::fill_n(destination, count, 0.0f);
    }
    static void copy(float* destination, const float* source, std::size_t count) noexcept {
        if (count) std::memmove(destination, source, count * sizeof(float));
    }
    static bool needsReverse(const float* destination, const float* source,
                             std::size_t count) noexcept {
        const auto d = reinterpret_cast<std::uintptr_t>(destination);
        const auto s = reinterpret_cast<std::uintptr_t>(source);
        return d > s && (d - s) / sizeof(float) < count;
    }
    static void negate(float* destination, const float* source, std::size_t count) noexcept {
        if (needsReverse(destination, source, count)) {
            for (auto n = count; n != 0; --n) destination[n - 1] = -source[n - 1];
        } else {
            for (std::size_t n = 0; n < count; ++n) destination[n] = -source[n];
        }
    }
    static void multiply(float* destination, float gain, std::size_t count) noexcept {
        for (std::size_t n = 0; n < count; ++n) destination[n] *= gain;
    }
    static void addWithMultiply(float* destination, const float* source, float gain,
                                std::size_t count) noexcept {
        if (needsReverse(destination, source, count)) {
            for (auto n = count; n != 0; --n) destination[n - 1] += source[n - 1] * gain;
        } else {
            for (std::size_t n = 0; n < count; ++n) destination[n] += source[n] * gain;
        }
    }
};
} // namespace cell::dsp
