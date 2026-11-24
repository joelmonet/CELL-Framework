// SPDX-License-Identifier: Apache-2.0
#pragma once
#include <algorithm>
#include <array>
#include <cmath>

namespace cell::dsp {
// Topology-preserving state-variable filter with two internal time steps.
// The caller supplies tan(pi*cutoff/(2*sampleRate)) and reciprocal Q.
class TwoStepStateVariable {
public:
  enum Response { lowPass, bandPass, highPass };
  void reset() noexcept {
    state_ = {};
    previous_ = 0;
  }
  float process(float input, float tangent, float damping,
                Response response) noexcept {
    input = std::isfinite(input) ? std::clamp(input, -32.f, 32.f) : 0;
    const float g = std::isfinite(tangent) ? std::clamp(tangent, 0.f, 4.f) : 0;
    const float k =
        std::isfinite(damping) ? std::clamp(damping, .05f, 4.f) : 1.41421356f;
    const float a1 = 1 / (1 + g * (g + k)), a2 = g * a1, a3 = g * a2;
    float result = 0;
    for (unsigned n = 0; n < 2; ++n) {
      const float x = n ? input : (input + previous_) * .5f, v3 = x - state_[1],
                  v1 = a1 * state_[0] + a2 * v3,
                  v2 = state_[1] + a2 * state_[0] + a3 * v3;
      state_[0] = 2 * v1 - state_[0];
      state_[1] = 2 * v2 - state_[1];
      result += (response == lowPass    ? v2
                 : response == bandPass ? v1
                                        : x - k * v1 - v2) *
                .5f;
    }
    previous_ = input;
    if (!std::isfinite(result)) {
      reset();
      return 0;
    }
    return result;
  }

private:
  std::array<float, 2> state_{};
  float previous_{};
};
} // namespace cell::dsp
