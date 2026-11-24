// SPDX-License-Identifier: Apache-2.0
#pragma once
#include <algorithm>
#include <array>
#include <cmath>

namespace cell::dsp {

// Fractional-rate sample-and-hold with continuous quantizer resolution. Clock
// phase is shared by stereo channels. Aliasing is intentional in this effect;
// use a proper resampler for transparent sample-rate conversion.
class FractionalReducer {
public:
  void reset() noexcept {
    phase_ = 0;
    held_ = {};
    first_ = true;
  }
  std::array<float, 2> process(std::array<float, 2> input, float ratio,
                               float levels) noexcept {
    ratio = std::isfinite(ratio) ? std::clamp(ratio, .00001f, 1.f) : 1;
    levels =
        std::isfinite(levels) ? std::clamp(levels, 1.f, 8388608.f) : 8388608;
    if (first_ || phase_ >= 1) {
      first_ = false;
      phase_ -= std::floor(phase_);
      for (unsigned c = 0; c < 2; ++c) {
        const float x =
            std::isfinite(input[c]) ? std::clamp(input[c], -32.f, 32.f) : 0;
        held_[c] = std::round(x * levels) / levels;
      }
    }
    phase_ += ratio;
    return held_;
  }

private:
  double phase_{};
  std::array<float, 2> held_{};
  bool first_ = true;
};

// Stereo state-variable low-pass, evaluated with two internal time steps.
// The caller supplies the tangent coefficient for twice the external rate.
class TwoStepLowPass {
public:
  void reset() noexcept { state_ = {}; }
  std::array<float, 2> process(std::array<float, 2> input, float g) noexcept {
    std::array<float, 2> result{};
    constexpr float k = 1.41421356237f;
    const float a1 = 1 / (1 + g * (g + k)), a2 = g * a1, a3 = g * a2;
    for (unsigned c = 0; c < 2; ++c)
      for (unsigned step = 0; step < 2; ++step) {
        auto &z = state_[c];
        const float v3 = input[c] - z[1], v1 = a1 * z[0] + a2 * v3,
                    v2 = z[1] + a2 * z[0] + a3 * v3;
        z[0] = 2 * v1 - z[0];
        z[1] = 2 * v2 - z[1];
        result[c] += v2 * .5f;
      }
    return result;
  }

private:
  std::array<std::array<float, 2>, 2> state_{};
};

// Four one-pole TPT stages with an algebraically solved linear feedback loop.
// Input saturation limits excitation without introducing an explicit feedback
// delay. The first stage provides the high-pass output used by creative FX.
class LadderHighPass {
public:
  void reset() noexcept { state_ = {}; }
  std::array<float, 2> process(std::array<float, 2> input, float g,
                               float resonance) noexcept {
    const float G = g / (1 + g), G2 = G * G, G3 = G2 * G, G4 = G3 * G,
                k = 3.95f * std::clamp(resonance, 0.f, 1.f);
    std::array<float, 2> output{};
    for (unsigned c = 0; c < 2; ++c)
      for (unsigned step = 0; step < 2; ++step) {
        auto &z = state_[c];
        const float x = std::tanh(input[c]),
                    history =
                        (1 - G) * (G3 * z[0] + G2 * z[1] + G * z[2] + z[3]);
        const float last = (G4 * x + history) / (1 + k * G4),
                    feed = x - k * last;
        float value = feed, first = 0;
        for (unsigned stage = 0; stage < 4; ++stage) {
          const float v = (value - z[stage]) * G, y = v + z[stage];
          z[stage] = y + v;
          value = y;
          if (!stage)
            first = y;
        }
        output[c] += (feed - first) * .5f;
      }
    return output;
  }

private:
  std::array<std::array<float, 4>, 2> state_{};
};

// First-order antiderivative integration of a morph between cubic soft
// saturation and hard limiting. All state is inline; drive/shape may vary.
class IntegratedClip {
public:
  void reset() noexcept { previous_ = 0; }
  float process(float input, float gain, float shape) noexcept {
    const double value = double(input) * gain, delta = value - previous_,
                 middle = (value + previous_) * .5;
    float soft, hard;
    if (std::abs(delta) < 1e-4 * (1 + std::abs(value))) {
      const float x = float(std::clamp(middle, -1., 1.));
      soft = 1.5f * (x - x * x * x / 3);
      hard = x;
    } else {
      soft =
          float(1.5 * (primitive(value, false) - primitive(previous_, false)) /
                delta);
      hard =
          float((primitive(value, true) - primitive(previous_, true)) / delta);
    }
    previous_ = value;
    return soft + std::clamp(shape, 0.f, 1.f) * (hard - soft);
  }

private:
  static double primitive(double x, bool hard) noexcept {
    const double a = std::abs(x);
    if (a > 1)
      return hard ? a - .5 : (2. / 3) * a - .25;
    return hard ? x * x * .5 : x * x * .5 - x * x * x * x / 12;
  }
  double previous_{};
};
} // namespace cell::dsp
