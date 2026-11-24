// SPDX-License-Identifier: Apache-2.0
#pragma once
#include <array>
#include <cell/audio.hpp>
#include <cmath>
#include <cstdint>
#include <limits>
#include <span>

namespace cell::dsp {
// A non-owning delay history. Only bind accesses storage ownership; reset
// invalidates old samples in constant time without clearing the backing store.
// The caller must keep the storage alive and give this view exclusive write
// access. Planar and stereo-interleaved layouts use the same processing API.
class DelayHistory {
public:
  enum Interpolation { nearest, linear, cubic };
  void bind(AudioBlock<float> storage) {
    if (storage.channelCount < 1 || storage.channelCount > 2 ||
        storage.frames < 8 ||
        storage.frames > unsigned(std::numeric_limits<int>::max()))
      throw std::invalid_argument("Invalid planar delay storage");
    std::array<float *, 2> proposed{};
    for (unsigned c = 0; c < storage.channelCount; ++c) {
      if (!storage.channel(c))
        throw std::invalid_argument("Null delay storage");
      proposed[c] = storage.channel(c);
    }
    channels_ = storage.channelCount;
    size_ = storage.frames;
    interleaved_ = {};
    planar_ = proposed;
    reset();
  }
  void bind(std::span<std::array<float, 2>> storage, unsigned channels) {
    if (channels < 1 || channels > 2 || storage.size() < 8 ||
        storage.size() > unsigned(std::numeric_limits<int>::max()))
      throw std::invalid_argument("Invalid interleaved delay storage");
    channels_ = channels;
    size_ = unsigned(storage.size());
    interleaved_ = storage;
    planar_ = {};
    reset();
  }
  void reset() noexcept { write_ = valid_ = 0; }
  std::size_t storageBytes() const noexcept {
    return std::size_t(size_) * (interleaved_.empty() ? channels_ : 2) *
           sizeof(float);
  }
  unsigned frames() const noexcept { return size_; }
  float read(unsigned channel, double delay,
             Interpolation interpolation = cubic) const noexcept {
    if (!size_ || !valid_)
      return 0;
    channel = channels_ == 1 ? 0 : std::min(channel, 1u);
    delay = std::isfinite(delay)
                ? std::clamp(delay, interpolation == cubic ? 2. : 1.,
                             double(size_ - 3))
            : interpolation == cubic ? 2.
                                     : 1.;
    double at = double(write_) - delay;
    if (at < 0)
      at += size_;
    if (at >= size_)
      at = 0;
    const unsigned index = unsigned(at);
    const float f = float(at - index);
    const auto sample = [&](int offset) {
      std::int64_t p = std::int64_t(index) + offset;
      if (p < 0)
        p += int(size_);
      else if (p >= int(size_))
        p -= int(size_);
      unsigned age = write_ >= unsigned(p) ? write_ - unsigned(p)
                                           : write_ + size_ - unsigned(p);
      if (!age || age > valid_)
        return 0.f;
      return interleaved_.empty() ? planar_[channel][p]
                                  : interleaved_[unsigned(p)][channel];
    };
    if (interpolation == nearest)
      return sample(f >= .5f ? 1 : 0);
    const float b = sample(0), c = sample(1);
    if (interpolation == linear)
      return b + (c - b) * f;
    const float a = sample(-1), d = sample(2);
    return b +
           .5f * f *
               (c - a +
                f * (2 * a - 5 * b + 4 * c - d + f * (3 * (b - c) + d - a)));
  }
  void push(std::array<float, 2> input) noexcept {
    if (!size_)
      return;
    for (unsigned c = 0; c < channels_; ++c) {
      const float x = std::isfinite(input[c]) ? input[c] : 0;
      if (interleaved_.empty())
        planar_[c][write_] = x;
      else
        interleaved_[write_][c] = x;
    }
    if (++write_ == size_)
      write_ = 0;
    if (valid_ < size_ - 1)
      ++valid_;
  }

private:
  std::array<float *, 2> planar_{};
  std::span<std::array<float, 2>> interleaved_;
  unsigned size_{}, channels_ = 2, write_{}, valid_{};
};
} // namespace cell::dsp
