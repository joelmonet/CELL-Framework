#pragma once

#include <cell/network_audio.hpp>

namespace cell::network_audio {

enum class PacketAdmission { accepted, invalid, late, outsideWindow, overlap };

struct ReorderDrain {
    std::size_t queued{};
    std::size_t missing{};
};

// The transport thread owns this window and is the sole producer of the playback buffer.
template<std::size_t Channels, std::size_t WindowFrames>
class ReorderBuffer {
    static_assert(Channels > 0 && WindowFrames > 0);

public:
    explicit ReorderBuffer(std::uint64_t firstSample) noexcept : nextSample_(firstSample) {}

    std::uint64_t nextSample() const noexcept { return nextSample_; }

    [[nodiscard]] PacketAdmission insert(std::uint64_t firstSample,
                                         std::span<const Frame<Channels>> frames) noexcept {
        if (frames.empty() || frames.size() > std::numeric_limits<std::uint64_t>::max() - firstSample)
            return PacketAdmission::invalid;
        if (firstSample < nextSample_) return PacketAdmission::late;
        const auto offset = firstSample - nextSample_;
        if (offset >= WindowFrames || frames.size() > WindowFrames - offset)
            return PacketAdmission::outsideWindow;

        for (std::size_t index = 0; index < frames.size(); ++index)
            if (present_[slot(firstSample + index)]) return PacketAdmission::overlap;
        for (std::size_t index = 0; index < frames.size(); ++index) {
            const auto position = slot(firstSample + index);
            frames_[position] = frames[index];
            present_[position] = true;
        }
        return PacketAdmission::accepted;
    }

    // Samples before concealBefore have missed their deadline. Stop at the first
    // unexpired gap, or when scratch space or playback capacity runs out.
    template<std::size_t PlaybackFrames>
    ReorderDrain pump(ReceiveBuffer<Channels, PlaybackFrames>& playback,
                      std::span<Frame<Channels>> scratch, std::uint64_t concealBefore) noexcept {
        ReorderDrain result;
        const auto limit = std::min({scratch.size(), playback.writableFrames(), WindowFrames});
        auto sample = nextSample_;
        while (result.queued < limit && sample < std::numeric_limits<std::uint64_t>::max()) {
            const auto position = slot(sample);
            if (!present_[position] && sample >= concealBefore) break;
            if (present_[position]) {
                scratch[result.queued] = frames_[position];
            } else {
                scratch[result.queued] = {};
                ++result.missing;
            }
            ++sample;
            ++result.queued;
        }

        if (result.queued && !playback.enqueue(scratch.first(result.queued))) return {};
        while (nextSample_ < sample) {
            present_[slot(nextSample_)] = false;
            ++nextSample_;
        }
        return result;
    }

private:
    static std::size_t slot(std::uint64_t sample) noexcept {
        return static_cast<std::size_t>(sample % WindowFrames);
    }

    std::array<Frame<Channels>, WindowFrames> frames_{};
    std::array<bool, WindowFrames> present_{};
    std::uint64_t nextSample_;
};

} // namespace cell::network_audio
