#pragma once

#include <algorithm>
#include <cmath>
#include <limits>

#include <cell/core.hpp>

namespace cell::network_audio {

template<std::size_t Channels>
using Frame = std::array<float, Channels>;

// One producer supplies decoded frames in stream order. One audio thread renders.
template<std::size_t Channels, std::size_t CapacityFrames>
class ReceiveBuffer {
    static_assert(Channels > 0 && CapacityFrames > 0);
    static_assert(CapacityFrames < std::numeric_limits<std::size_t>::max() / 2);
    static_assert(std::atomic<std::size_t>::is_always_lock_free);

public:
    explicit ReceiveBuffer(std::size_t prefillFrames, unsigned maxCorrectionPpm = 0) noexcept
        : prefillFrames_(std::clamp(prefillFrames, std::size_t{1}, CapacityFrames)),
          maxCorrection_(CapacityFrames >= 2 ? std::min(maxCorrectionPpm, 5000u) / 1'000'000.0 : 0.0) {}

    // Producer only. The consumer can only increase this budget.
    std::size_t writableFrames() const noexcept { return frames_.freeCapacity(); }
    int correctionPpm() const noexcept { return correctionPpm_.load(std::memory_order_relaxed); }

    // Producer only. Reject the entire batch when there is insufficient room.
    [[nodiscard]] bool enqueue(std::span<const Frame<Channels>> frames) noexcept {
        return frames_.pushBatch(frames);
    }

    // Consumer only. Return output frames derived from queued audio, excluding underrun silence.
    std::size_t render(std::span<Frame<Channels>> output) noexcept {
        if (output.empty()) return 0;
        const auto ready = frames_.available();
        if (maxCorrection_ > 0.0) return renderCorrected(output, ready);
        if (ready >= prefillFrames_) playing_ = true;
        const auto count = playing_ ? std::min(ready, output.size()) : 0;
        for (auto& frame : output.first(count)) frames_.pop(frame);
        std::ranges::fill(output.subspan(count), Frame<Channels>{});
        if (count < output.size()) playing_ = false;
        return count;
    }

private:
    std::size_t renderCorrected(std::span<Frame<Channels>> output, std::size_t ready) noexcept {
        const auto target = std::max(prefillFrames_, std::size_t{2});
        if (!playing_ && ready >= target) {
            frames_.pop(current_);
            frames_.pop(next_);
            ready -= 2;
            playing_ = true;
        }

        const double error = (static_cast<double>(ready) - static_cast<double>(target)) /
            static_cast<double>(target);
        const double desired = std::clamp(0.01 * error, -maxCorrection_, maxCorrection_);
        const double weight = std::min(1.0, static_cast<double>(output.size()) / 4800.0);
        if (playing_) correction_ = std::lerp(correction_, desired, weight);

        std::size_t count = 0;
        while (playing_ && count < output.size()) {
            while (phase_ >= 1.0 && ready) {
                current_ = next_;
                frames_.pop(next_);
                --ready;
                phase_ -= 1.0;
            }
            if (phase_ >= 1.0) {
                playing_ = false;
                phase_ = correction_ = 0.0;
                break;
            }
            for (std::size_t channel = 0; channel < Channels; ++channel)
                output[count][channel] = std::lerp(current_[channel], next_[channel],
                                                   static_cast<float>(phase_));
            ++count;
            phase_ += 1.0 + correction_;
        }
        std::ranges::fill(output.subspan(count), Frame<Channels>{});
        correctionPpm_.store(static_cast<int>(correction_ * 1'000'000.0), std::memory_order_relaxed);
        return count;
    }

    SpscQueue<Frame<Channels>, CapacityFrames + 1> frames_;
    const std::size_t prefillFrames_;
    const double maxCorrection_;
    Frame<Channels> current_{}, next_{};
    double phase_{}, correction_{};
    static_assert(std::atomic<int>::is_always_lock_free);
    std::atomic<int> correctionPpm_{};
    bool playing_ = false;
};

} // namespace cell::network_audio
