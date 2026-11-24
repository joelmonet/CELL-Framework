// SPDX-License-Identifier: Apache-2.0
#pragma once
#include <algorithm>
#include <cmath>

namespace cell::dsp {
// Linked peak follower with optional amplitude-statistics normalization. The
// adaptive range follows the input rather than an arbitrary absolute gate.
class AdaptiveEnvelope {
public:
  void prepare(double sampleRate) noexcept {
    rate_ = std::isfinite(sampleRate) ? std::clamp(sampleRate, 8000., 384000.)
                                      : 48000;
    attackMs_ = releaseMs_ = windowMs_ = -1;
    reset();
    settings(6.1f, 5.3f, 11.19032f, true);
  }
  void reset() noexcept {
    level_ = mean_ = square_ = 0;
    cycle_ = 0;
    lower_ = 0;
    span_ = 1;
  }
  void settings(float attackMs, float releaseMs, float sensitivityDb,
                bool adaptive, float windowMs = 70.9f,
                float ceiling = 1) noexcept {
    attackMs = finite(attackMs, .1f, 1000, 6.1f);
    releaseMs = finite(releaseMs, .1f, 3000, 5.3f);
    sensitivityDb = finite(sensitivityDb, -24, 36, 11.19032f);
    windowMs = finite(windowMs, 1, 10000, 70.9f);
    if (attackMs != attackMs_) {
      attackMs_ = attackMs;
      attack_ =
          float(1 - std::exp(-1.203972804325936 / (rate_ * .001 * attackMs)));
    }
    if (releaseMs != releaseMs_) {
      releaseMs_ = releaseMs;
      release_ =
          float(1 - std::exp(-1.203972804325936 / (rate_ * .001 * releaseMs)));
    }
    if (windowMs != windowMs_) {
      windowMs_ = windowMs;
      follow_ = float(1 - std::exp(-1 / (rate_ * .001 * windowMs)));
    }
    if (sensitivityDb != sensitivityDb_) {
      sensitivityDb_ = sensitivityDb;
      gain_ = std::exp2(sensitivityDb / 6.020599913f);
    }
    adaptive_ = adaptive;
    ceiling_ = finite(ceiling, .0001f, 16, 1);
  }
  float process(float left, float right) noexcept {
    const float peak = std::max(std::abs(finite(left, -32, 32, 0)),
                                std::abs(finite(right, -32, 32, 0))) *
                       gain_;
    level_ += (peak > level_ ? attack_ : release_) * (peak - level_);
    mean_ += follow_ * (level_ - mean_);
    square_ += follow_ * (level_ * level_ - square_);
    if (!adaptive_)
      return std::clamp(level_ / ceiling_, 0.f, 1.f);
    if (!cycle_) {
      const float sigma = std::sqrt(std::max(0.f, square_ - mean_ * mean_));
      lower_ = std::max(0.f, mean_ - sigma);
      span_ = std::max(.0001f, 2 * sigma + mean_ * .15f);
    }
    cycle_ = (cycle_ + 1) & 15;
    return level_ < .00001f ? 0
                            : std::clamp((level_ - lower_) / span_, 0.f, 1.f);
  }

private:
  static float finite(float value, float lo, float hi,
                      float fallback) noexcept {
    return std::isfinite(value) ? std::clamp(value, lo, hi) : fallback;
  }
  double rate_ = 48000;
  float level_{}, mean_{}, square_{}, lower_{}, span_ = 1;
  float attack_{}, release_{}, follow_{}, gain_ = 1, ceiling_ = 1;
  float attackMs_ = -1, releaseMs_ = -1, sensitivityDb_ = -100, windowMs_ = -1;
  unsigned cycle_{};
  bool adaptive_ = true;
};

// Hysteretic audio-triggered envelope. Retriggering ramps from the current
// level; attack and release do not depend on the host's buffer boundaries.
class TriggeredEnvelope {
public:
  void reset() noexcept {
    value_ = 0;
    gate_ = false;
  }
  float process(float detector, double rate, float attackMs, float releaseMs,
                float on = .016155f, float off = .009025f) noexcept {
    if (!std::isfinite(detector))
      detector = 0;
    on = std::isfinite(on) ? std::clamp(on, .00001f, 1.f) : .016155f;
    off = std::isfinite(off) ? std::clamp(off, 0.f, on * .999f) : on * .5f;
    if (!gate_ && detector >= on)
      gate_ = true;
    else if (gate_ && detector <= off)
      gate_ = false;
    rate = std::isfinite(rate) ? std::clamp(rate, 8000., 384000.) : 48000;
    attackMs =
        std::isfinite(attackMs) ? std::clamp(attackMs, .1f, 10000.f) : 3000;
    releaseMs =
        std::isfinite(releaseMs) ? std::clamp(releaseMs, .1f, 10000.f) : 100;
    value_ += float((gate_ ? 1 : -1) /
                    (rate * .001 * (gate_ ? attackMs : releaseMs)));
    value_ = std::clamp(value_, 0.f, 1.f);
    return value_;
  }

private:
  float value_{};
  bool gate_{};
};
} // namespace cell::dsp
