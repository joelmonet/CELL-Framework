// SPDX-License-Identifier: Apache-2.0
#pragma once
#include <algorithm>
#include <array>
#include <cmath>

namespace cell::dsp {
// Four TPT one-poles with an algebraically solved feedback connection. The
// drive is bounded before the loop; two interpolated substeps reduce aliasing.
class TptLadder {
public:
  enum Response { lowPass, bandPass };
  class Excitation {
    friend class TptLadder;
    std::array<std::array<float, 2>, 2> samples_{};
    std::array<float, 2> input_{};
  };
  void reset() noexcept {
    state_ = {};
    previous_ = {};
  }
  std::array<float, 2> process(std::array<float, 2> input, float tangent,
                               float resonance, Response response,
                               unsigned channels = 2) noexcept {
    return processExcited(excitation(input, channels), tangent, resonance,
                          response, channels);
  }
  // Parallel filters with the same input history can reuse this calculation.
  // Both processExcited calls update their own previous-input histories.
  Excitation excitation(std::array<float, 2> input,
                        unsigned channels = 2) const noexcept {
    channels = channels == 1 ? 1 : 2;
    Excitation result;
    for (unsigned c = 0; c < channels; ++c) {
      const float now =
          std::isfinite(input[c]) ? std::clamp(input[c], -32.f, 32.f) : 0;
      result.input_[c] = now;
      result.samples_[c] = {std::tanh((now + previous_[c]) * .5f),
                            std::tanh(now)};
    }
    return result;
  }
  std::array<float, 2> processExcited(const Excitation &input, float tangent,
                                      float resonance, Response response,
                                      unsigned channels = 2) noexcept {
    const float g = std::isfinite(tangent) ? std::clamp(tangent, 0.f, 4.f) : 0,
                G = g / (1 + g), G2 = G * G, G3 = G2 * G, G4 = G3 * G;
    const float k =
        3.95f *
        (std::isfinite(resonance) ? std::clamp(resonance, 0.f, 1.f) : 0);
    channels = channels == 1 ? 1 : 2;
    std::array<float, 2> result{};
    for (unsigned c = 0; c < channels; ++c) {
      for (unsigned step = 0; step < 2; ++step) {
        auto &z = state_[c];
        const float x = input.samples_[c][step];
        const float history =
            (1 - G) * (G3 * z[0] + G2 * z[1] + G * z[2] + z[3]);
        const float last = (G4 * x + history) / (1 + k * G4);
        float value = x - k * last, second = 0;
        for (unsigned n = 0; n < 4; ++n) {
          const float v = (value - z[n]) * G, y = v + z[n];
          z[n] = y + v;
          value = y;
          if (n == 1)
            second = y;
        }
        result[c] += (response == lowPass ? value : 2 * (second - value)) * .5f;
      }
      previous_[c] = input.input_[c];
    }
    if (channels == 1)
      result[1] = result[0];
    return result;
  }

private:
  std::array<std::array<float, 4>, 2> state_{};
  std::array<float, 2> previous_{};
};
} // namespace cell::dsp
