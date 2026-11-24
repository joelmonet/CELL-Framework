// Original Cell Framework implementation; see LICENSE.
#pragma once
#include <cell/owned_audio.hpp>
#include <climits>
#include <concepts>

namespace cell {
// Processing dimensions fit signed 32-bit host blocks. Source/editing storage
// remains independently 64-bit in OwnedAudio. Referencing host memory allocates
// nothing; owning allocations belong to preparation.
template <class Sample = float>
class ProcessBuffer : public OwnedAudio<Sample> {
  using Base = OwnedAudio<Sample>;
  template <std::integral C, std::integral F>
  static void dimensions(C channels, F frames) {
    if constexpr (std::is_signed_v<C>)
      if (channels < 0)
        throw std::invalid_argument("Negative processing channel count");
    if constexpr (std::is_signed_v<F>)
      if (frames < 0)
        throw std::invalid_argument("Negative processing frame count");
    if (std::uintmax_t(channels) > INT_MAX || std::uintmax_t(frames) > INT_MAX)
      throw std::length_error(
          "Processing block dimensions exceed signed host range");
  }

public:
  ProcessBuffer() = default;
  template <std::integral C, std::integral F>
  ProcessBuffer(C channels, F frames) {
    resize(channels, frames);
  }
  template <std::integral C, std::integral F>
  ProcessBuffer(Sample *const *source, C channels, F frames) {
    setDataToReferTo(source, channels, frames);
  }
  ProcessBuffer(const ProcessBuffer &) = default;
  ProcessBuffer(ProcessBuffer &&) noexcept = default;
  ProcessBuffer &operator=(const ProcessBuffer &) = default;
  ProcessBuffer &operator=(ProcessBuffer &&) noexcept = default;
  ProcessBuffer &operator=(const Base &other) {
    dimensions(other.channelCount(), other.frameCount());
    Base::operator=(other);
    return *this;
  }
  int getNumChannels() const noexcept { return int(Base::channelCount()); }
  int getNumSamples() const noexcept { return int(Base::frameCount()); }
  template <std::integral C, std::integral F>
  void resize(C channels, F frames) {
    setSize(channels, frames);
  }
  template <std::integral C, std::integral F>
  void setSize(C channels, F frames, bool keep = false, bool clear = true,
               bool reuse = false) {
    dimensions(channels, frames);
    Base::setSize(std::size_t(channels), std::size_t(frames), keep, clear,
                  reuse);
  }
  template <std::integral C, std::integral F>
  void setDataToReferTo(Sample *const *source, C channels, F frames) {
    dimensions(channels, frames);
    Base::setDataToReferTo(source, std::size_t(channels), std::size_t(frames));
  }
  template <class Other>
  void makeCopyOf(const OwnedAudio<Other> &source, bool reuse = false) {
    dimensions(source.channelCount(), source.frameCount());
    Base::makeCopyOf(source, reuse);
  }
  void swap(ProcessBuffer &other) noexcept { Base::swap(other); }
  void swap(Base &other) {
    dimensions(other.channelCount(), other.frameCount());
    Base::swap(other);
  }
  void applyGainRamp(int start, int count, Sample first, Sample last) {
    for (int c = 0; c < getNumChannels(); ++c)
      Base::applyGainRamp(c, start, count, first, last);
  }
  using Base::applyGainRamp;
  Sample getMagnitude(int channel, int start, int count) const {
    if (start < 0 || count < 0 || channel < 0 || channel >= getNumChannels() ||
        start > getNumSamples() - count)
      throw std::out_of_range("Processing peak range");
    Sample peak{};
    const auto *p = Base::getReadPointer(channel, start);
    for (int i = 0; i < count; ++i)
      peak = std::max(peak, std::abs(p[i]));
    return peak;
  }
};
} // namespace cell
