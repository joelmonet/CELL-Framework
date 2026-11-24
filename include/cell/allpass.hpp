// SPDX-License-Identifier: Apache-2.0
#pragma once
#include <algorithm>
#include <array>
#include <cmath>

namespace cell::dsp {
// H(z)=(z^-1-a)/(1-a*z^-1). Each stage has unit magnitude for a fixed
// coefficient; cascading stages changes phase without a delay-line allocation.
template <unsigned Maximum = 16> class AllpassCascade {
  static_assert(Maximum > 0);

public:
  void reset() noexcept { state_ = {}; }
  float process(float input, float coefficient, unsigned stages) noexcept {
    float x = std::isfinite(input) ? input : 0;
    const float a =
        std::isfinite(coefficient) ? std::clamp(coefficient, -.995f, .995f) : 0;
    stages = std::clamp(stages, 1u, Maximum);
    for (unsigned n = 0; n < stages; ++n) {
      const float y = state_[n] - a * x;
      state_[n] = x + a * y;
      x = y;
    }
    return x;
  }

private:
  std::array<float, Maximum> state_{};
};
} // namespace cell::dsp
