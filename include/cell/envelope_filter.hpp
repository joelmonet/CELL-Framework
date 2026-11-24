// SPDX-License-Identifier: Apache-2.0
#pragma once
#include <algorithm>
#include <array>
#include <cmath>
#include <numbers>

namespace cell::dsp {

// An independently implemented, causal stereo envelope filter. All storage is
// inline. Settings and mode changes never allocate or clear a delay buffer.
struct EnvelopeFilterSettings {
  int type = 9, floor = 1, clip = 0;
  float cutoff = 1200, resonance = 55, gain = 0, envelope = 36;
  float attack = 8, release = 220, sensitivity = 12, drive = 3, sweep = 24,
        stereo = 90;
};

class EnvelopeFilter {
public:
  void prepare(double sampleRate) noexcept {
    rate_ = std::isfinite(sampleRate) ? std::clamp(sampleRate, 8000., 384000.)
                                      : 48000.;
    smoothing_ = float(1 - std::exp(-1 / (rate_ * .015)));
    dcCoefficient_ = float(1 - std::exp(-2 * std::numbers::pi * 5 / rate_));
    reset();
  }
  void reset() noexcept {
    stages_ = {};
    onePole_ = {};
    low18_ = {};
    previous_ = {};
    previousDrive_ = {};
    dc_ = {};
    coefficients_ = {};
    envelope_ = 0;
    phase_ = 0;
    tick_ = 0;
    fresh_ = initialCoefficients_ = true;
  }
  void setSettings(EnvelopeFilterSettings s,
                   bool feedbackSafe = false) noexcept {
    s.type = std::clamp(s.type, 0, 9);
    s.floor = std::clamp(s.floor, 0, 3);
    s.clip = std::clamp(s.clip, 0, 1);
    s.cutoff = finite(s.cutoff, 20, 20000, 1200);
    s.resonance = finite(s.resonance, 0, 100, 55);
    s.gain = finite(s.gain, -18, 18, 0);
    s.envelope = finite(s.envelope, -72, 72, 36);
    s.attack = finite(s.attack, .1f, 1000, 8);
    s.release = finite(s.release, 5, 3000, 220);
    s.sensitivity = finite(s.sensitivity, -24, 36, 12);
    s.drive = finite(s.drive, 0, 24, 3);
    s.sweep = finite(s.sweep, 0, 72, 24);
    s.stereo = finite(s.stereo, 0, 180, 90);
    settings_ = s;
    safe_ = feedbackSafe;
    targets_ = {std::log2(s.cutoff),
                s.resonance,
                std::exp2(s.gain / 6.020599913f),
                s.envelope,
                std::exp2(s.sensitivity / 6.020599913f),
                std::exp2(s.drive / 6.020599913f),
                s.sweep,
                s.stereo};
    attack_ = float(1 - std::exp(-1 / (rate_ * .001 * s.attack)));
    release_ = float(1 - std::exp(-1 / (rate_ * .001 * s.release)));
    if (fresh_) {
      values_ = targets_;
      weights_ = {};
      weights_[s.type] = 1;
      hard_ = float(s.clip);
    }
  }
  // A linked detector keeps level-dependent cutoff movement coherent in
  // stereo. A delay can supply its dry input separately from the repeats.
  std::array<float, 2> processSample(float left, float right, float speed,
                                     float detectorLeft, float detectorRight,
                                     float motion = 1) noexcept {
    if (fresh_) {
      setSettings(settings_, safe_);
      fresh_ = false;
    }
    left = finite(left, -32, 32, 0);
    right = finite(right, -32, 32, 0);
    const float peak = std::max(std::abs(finite(detectorLeft, -32, 32, 0)),
                                std::abs(finite(detectorRight, -32, 32, 0)));
    envelope_ += (peak > envelope_ ? attack_ : release_) * (peak - envelope_);
    for (unsigned j = 0; j < values_.size(); ++j)
      values_[j] += smoothing_ * (targets_[j] - values_[j]);
    for (unsigned j = 0; j < weights_.size(); ++j)
      weights_[j] +=
          smoothing_ * ((int(j) == settings_.type ? 1.f : 0.f) - weights_[j]);
    hard_ += smoothing_ * (float(settings_.clip) - hard_);
    if (!tick_) {
      constexpr float floors[]{20, 100, 250, 500};
      const float minimum =
          std::min(floors[settings_.floor], float(rate_ * .42));
      const float follower =
          2 * std::clamp(envelope_ * values_[4], 0.f, 1.f) - 1;
      for (unsigned c = 0; c < 2; ++c) {
        const double wave = std::sin(2 * std::numbers::pi * phase_ +
                                     c * values_[7] * std::numbers::pi / 180);
        const float pitch =
            values_[0] + (values_[3] * follower +
                          values_[6] * float(wave) * finite(motion, 0, 1, 1)) /
                             24;
        const float hz =
            std::clamp(std::exp2(pitch), minimum, float(rate_ * .42));
        const float g = float(std::tan(std::numbers::pi * hz / (2 * rate_)));
        const float q = .70710678f * std::exp2(values_[1] * .04f);
        auto &k = coefficients_[c];
        const float damping = 1 / q;
        if (initialCoefficients_) {
          k.g = g;
          k.k = damping;
        }
        k.dg = (g - k.g) / 16;
        k.dk = (damping - k.k) / 16;
      }
      initialCoefficients_ = false;
    }
    tick_ = (tick_ + 1) & 15;
    std::array<float, 2> output{};
    for (unsigned c = 0; c < 2; ++c) {
      auto &k = coefficients_[c];
      k.g += k.dg;
      k.k += k.dk;
      const float input = c ? right : left;
      const float drive = values_[5], normalizer = safe_ ? drive : 1.f;
      const float resonanceGuard = safe_ ? std::min(1.f, k.k) : 1.f;
      const auto driven = [&](float x) {
        const float v = x * drive;
        // Integrate the piecewise polynomial waveshapers over each
        // substep instead of evaluating an aliased point clip.
        const double old = previousDrive_[c], delta = double(v) - old;
        const float hard =
            std::abs(delta) < 1e-4 * (1 + std::abs(v))
                ? std::clamp(float((v + old) * .5), -1.f, 1.f)
                : float((primitive(v, true) - primitive(old, true)) / delta);
        const float soft =
            std::abs(delta) < 1e-4 * (1 + std::abs(v))
                ? softClip(float((v + old) * .5))
                : float((primitive(v, false) - primitive(old, false)) / delta);
        previousDrive_[c] = v;
        // At zero drive the separate distortion stage is transparent.
        const float blend = std::min(1.f, (drive - 1) * 8);
        return (x + (soft + hard_ * (hard - soft) - x) * blend) / normalizer;
      };
      float sum = 0;
      // Two causal internal substeps improve nonlinear/filter behaviour
      // at high cutoff. This is not a claim of alias-free hard clipping.
      for (int sub = 0; sub < 2; ++sub) {
        const float x = driven(sub ? input : (input + previous_[c]) * .5f);
        const auto first = stages_[c][0].process(x, k.g, k.k);
        const auto second = stages_[c][1].process(first[0], k.g, 1.41421356f);
        const float a = k.g / (1 + k.g), v = (x - onePole_[c]) * a,
                    low = v + onePole_[c];
        onePole_[c] = low + v;
        const float v18 = (first[0] - low18_[c]) * a, low18 = v18 + low18_[c];
        low18_[c] = low18 + v18;
        const float low12 = first[0] * resonanceGuard,
                    high = first[2] * resonanceGuard, bp = first[1];
        const float eq = (x + (values_[2] - 1) * bp) /
                         (safe_ ? std::max(1.f, values_[2]) : 1.f);
        const std::array<float, 10> variants{
            bp,
            high,
            low18 * resonanceGuard,
            low12,
            low,
            eq,
            softClip(low12 * 1.5f) / 1.5f,
            softClip(second[0] * resonanceGuard * 1.5f) / 1.5f,
            softClip(high * 1.5f) / 1.5f,
            softClip(bp * 1.5f) / 1.5f};
        for (unsigned type = 0; type < variants.size(); ++type)
          sum += variants[type] * weights_[type] * .5f;
      }
      previous_[c] = input;
      dc_[c] += dcCoefficient_ * (sum - dc_[c]);
      output[c] = sum - dc_[c];
      if (!std::isfinite(output[c])) {
        stages_[c] = {};
        onePole_[c] = low18_[c] = dc_[c] = 0;
        output[c] = 0;
      }
    }
    phase_ += finite(speed, 0, 80, 0) / rate_;
    if (phase_ >= 1)
      phase_ -= 1;
    return output;
  }
  std::array<float, 2> processSample(float left, float right,
                                     float speed) noexcept {
    return processSample(left, right, speed, left, right);
  }

private:
  static float finite(float x, float low, float high, float fallback) noexcept {
    return std::isfinite(x) ? std::clamp(x, low, high) : fallback;
  }
  static float softClip(float x) noexcept {
    x = std::clamp(x, -1.f, 1.f);
    return x - x * x * x / 3;
  }
  static double primitive(double x, bool hard) noexcept {
    const double a = std::abs(x);
    if (a > 1)
      return hard ? a - .5 : (2. / 3) * a - .25;
    return hard ? x * x * .5 : x * x * .5 - x * x * x * x / 12;
  }
  struct Stage {
    float z1{}, z2{};
    std::array<float, 3> process(float x, float g, float k) noexcept {
      const float a1 = 1 / (1 + g * (g + k)), a2 = g * a1, a3 = g * a2;
      const float v3 = x - z2, v1 = a1 * z1 + a2 * v3,
                  v2 = z2 + a2 * z1 + a3 * v3;
      z1 = 2 * v1 - z1;
      z2 = 2 * v2 - z2;
      return {v2, k * v1, x - k * v1 - v2};
    }
  };
  struct Coefficient {
    float g{}, k{}, dg{}, dk{};
  };
  double rate_ = 48000, phase_{};
  float smoothing_{}, dcCoefficient_{}, attack_{}, release_{}, envelope_{},
      hard_{};
  unsigned tick_{};
  bool fresh_ = true, safe_{}, initialCoefficients_ = true;
  EnvelopeFilterSettings settings_;
  std::array<float, 8> values_{}, targets_{};
  std::array<float, 10> weights_{};
  std::array<std::array<Stage, 2>, 2> stages_{};
  std::array<float, 2> onePole_{}, low18_{}, previous_{}, previousDrive_{},
      dc_{};
  std::array<Coefficient, 2> coefficients_{};
};
} // namespace cell::dsp
