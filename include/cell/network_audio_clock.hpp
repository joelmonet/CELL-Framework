#pragma once

#include <chrono>

#include <cell/network_audio.hpp>

namespace cell::network_audio {

// Map elapsed monotonic time since the stream anchor to an exclusive sender
// sample deadline. Account for the playback queue lead when choosing the delay.
class PlayoutClock {
public:
    PlayoutClock(std::uint64_t firstSample, std::uint32_t sampleRate,
                 std::chrono::nanoseconds delay) noexcept
        : firstSample_(firstSample), sampleRate_(sampleRate),
          delay_(std::max(delay, std::chrono::nanoseconds::zero())) {}

    std::uint64_t concealBefore(std::chrono::nanoseconds elapsed) const noexcept {
        if (elapsed <= delay_ || sampleRate_ == 0) return firstSample_;

        constexpr std::uint64_t nanosPerSecond = 1'000'000'000;
        constexpr auto maxSample = std::numeric_limits<std::uint64_t>::max();
        const auto active = static_cast<std::uint64_t>((elapsed - delay_).count());
        const auto seconds = active / nanosPerSecond;
        const auto fractionalFrames = (active % nanosPerSecond) * sampleRate_ / nanosPerSecond;
        const auto remaining = maxSample - firstSample_;
        if (seconds > remaining / sampleRate_) return maxSample;
        const auto wholeFrames = seconds * sampleRate_;
        if (fractionalFrames > remaining - wholeFrames) return maxSample;
        return firstSample_ + wholeFrames + fractionalFrames;
    }

private:
    std::uint64_t firstSample_;
    std::uint32_t sampleRate_;
    std::chrono::nanoseconds delay_;
};

} // namespace cell::network_audio
