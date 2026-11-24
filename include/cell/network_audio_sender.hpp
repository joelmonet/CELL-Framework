#pragma once

#include <cell/network_audio_packet.hpp>

namespace cell::network_audio {

// One audio callback supplies PCM and one transport thread encodes packets.
template<std::size_t Channels, std::size_t CapacityFrames>
class Sender {
    static_assert(Channels > 0 && Channels <= 32 && CapacityFrames > 0);
    static_assert(CapacityFrames < std::numeric_limits<std::size_t>::max() / 2);
    static_assert(std::atomic<std::size_t>::is_always_lock_free);

public:
    Sender(std::uint32_t sampleRate, std::uint64_t firstSample) noexcept
        : sampleRate_(sampleRate), nextSample_(firstSample) {}

    // Audio callback only. A full queue drops the whole block, but the sample clock advances.
    [[nodiscard]] bool capture(std::span<const Frame<Channels>> input) noexcept {
        if (!detail::validPacket<Channels>(sampleRate_, nextSample_, 1) ||
            input.size() > std::numeric_limits<std::uint64_t>::max() - nextSample_) return false;
        auto sample = nextSample_;
        nextSample_ += input.size();
        if (input.size() > frames_.freeCapacity()) return false;
        for (const auto& frame : input) frames_.push({sample++, frame});
        return true;
    }

    // Transport thread only. Empty means no queued audio. The transport owns the returned packet.
    [[nodiscard]] std::vector<std::byte> takePacket() {
        if (!pending_) {
            StampedFrame first;
            if (!frames_.pop(first)) return {};
            pending_ = first;
        }

        std::array<Frame<Channels>, (maxPacketBytes - detail::headerBytes) / sizeof(float) / Channels> audio;
        const auto firstSample = pending_->sample;
        std::size_t count = 0;
        do {
            auto& frame = audio[count++];
            frame = pending_->audio;
            for (auto& value : frame)
                if (!std::isfinite(value)) value = 0.0f;
            pending_.reset();
            if (count == audio.size()) break;
            StampedFrame next;
            if (frames_.pop(next)) pending_ = next;
        } while (pending_ && pending_->sample == firstSample + count);
        return encodePacket<Channels>(sampleRate_, firstSample, std::span(audio).first(count));
    }

private:
    struct StampedFrame {
        std::uint64_t sample;
        Frame<Channels> audio;
    };

    SpscQueue<StampedFrame, CapacityFrames + 1> frames_;
    const std::uint32_t sampleRate_;
    std::uint64_t nextSample_;
    std::optional<StampedFrame> pending_;
};

} // namespace cell::network_audio
