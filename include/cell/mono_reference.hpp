// SPDX-License-Identifier: Apache-2.0
#pragma once
#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>

namespace cell::dsp {
// Causal dominant-channel selection for a shared stereo pitch clock. Summing
// antiphase input can erase a valid reference; choosing a whole block's energy
// lets future input affect earlier decisions. This selector uses each sample
// once, then holds an established reference through modest level differences.
class MonoReference {
public:
  void prepare(double rate) noexcept {
    rate = std::isfinite(rate) ? std::clamp(rate, 8000., 384000.) : 48000.;
    coefficient_ = float(1 - std::exp(-1 / (rate * .010)));
    hold_ = std::max(1u, unsigned(std::ceil(rate * .012)));
    reset();
  }
  void reset() noexcept {
    power_ = {};
    selected_ = waiting_ = 0;
    acquired_ = false;
  }
  unsigned process(float left, float right, unsigned channels = 2) noexcept {
    const std::array x{
        std::isfinite(left) ? std::clamp(left, -32.f, 32.f) : 0.f,
        std::isfinite(right) ? std::clamp(right, -32.f, 32.f) : 0.f};
    for (unsigned c = 0; c < 2; ++c)
      power_[c] += coefficient_ * (x[c] * x[c] - power_[c]);
    if (channels < 2) {
      selected_ = waiting_ = 0;
      return 0;
    }
    if (!acquired_) {
      if (std::max(power_[0], power_[1]) > 1e-9f) {
        selected_ = power_[1] > power_[0] ? 1u : 0u;
        acquired_ = true;
      }
      return selected_;
    }
    const auto other = 1 - selected_;
    if (power_[other] > std::max(1e-9f, power_[selected_] * 8)) {
      if (++waiting_ >= hold_) {
        selected_ = other;
        waiting_ = 0;
      }
    } else
      waiting_ = 0;
    return selected_;
  }
  unsigned channel() const noexcept { return selected_; }

private:
  std::array<float, 2> power_{};
  float coefficient_{};
  unsigned selected_{}, waiting_{}, hold_ = 576;
  bool acquired_ = false;
};
} // namespace cell::dsp
