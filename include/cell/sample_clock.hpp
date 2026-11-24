// Original Cell Framework implementation; see LICENSE.
#pragma once
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <limits>
#include <stdexcept>

namespace cell::audio {
// Compile a half-open time boundary to its first included source sample.
// True fractional boundaries round upward. Representation error around an
// integral frame is resolved consistently across adjacent ranges, long song
// origins, and platforms where long double has only double precision.
// Control/worker-thread conversion; prepare integer indices before playback.
inline std::int64_t sampleBoundary(double time, long double origin,
                                   double rate) {
  if (!std::isfinite(time) || !std::isfinite(origin) || !std::isfinite(rate) ||
      rate <= 0)
    throw std::invalid_argument("Invalid audio sample clock");
  const long double wanted = (static_cast<long double>(time) - origin) * rate;
  const auto uncertainty =
      8.L * std::numeric_limits<double>::epsilon() *
      std::max(std::abs(static_cast<long double>(time)), std::abs(origin)) *
      rate;
  if (uncertainty >= .5L)
    throw std::out_of_range(
        "Audio timestamp cannot resolve an individual source sample");
  const auto nearest = std::round(wanted);
  const auto boundary =
      std::abs(wanted - nearest) <= std::max(1.e-7L, uncertainty)
          ? nearest
          : std::ceil(wanted);
  if (!std::isfinite(boundary) || boundary < -std::ldexp(1.L, 63) ||
      boundary >= std::ldexp(1.L, 63))
    throw std::out_of_range(
        "Audio sample boundary exceeds signed 64-bit addressing");
  return static_cast<std::int64_t>(boundary);
}
} // namespace cell::audio
