#pragma once

#include <cell/network_audio_clock.hpp>
#include <cell/network_audio_packet.hpp>
#include <cell/network_audio_reorder.hpp>

namespace cell::network_audio {

struct ReceiverConfig {
    std::uint64_t firstSample{};
    std::uint32_t sampleRate{};
    std::chrono::nanoseconds arrivalBudget{};
    std::size_t prefillFrames{};
    unsigned maxCorrectionPpm{};
    bool startAtFirstPacket{}; // Use the first admissible packet's sample as the stream origin.
};

struct ReceiverStats {
    std::size_t acceptedPackets{};
    std::size_t invalidPackets{};
    std::size_t latePackets{};
    std::size_t outsideWindowPackets{};
    std::size_t overlappingPackets{};
    std::size_t concealedSourceFrames{};
    std::size_t unfilledOutputFrames{};
};

// One negotiated stream. Construct before starting the transport and audio threads.
template<std::size_t Channels, std::size_t WindowFrames, std::size_t PlaybackFrames>
class Receiver {
    static_assert(Channels > 0 && Channels <= 32);

public:
    explicit Receiver(ReceiverConfig config) noexcept
        : sampleRate_(config.sampleRate),
          arrivalBudget_(config.arrivalBudget),
          playback_(config.prefillFrames, config.maxCorrectionPpm) {
        if (!config.startAtFirstPacket)
            timeline_.emplace(config.firstSample, sampleRate_, arrivalBudget_);
    }

    // Transport thread. Send a complete datagram from the negotiated peer.
    [[nodiscard]] PacketAdmission receive(std::span<const std::byte> datagram) noexcept {
        const auto decoded = decodePacket<Channels>(datagram, sampleRate_, scratch_);
        auto admission = PacketAdmission::invalid;
        if (decoded) {
            if (!timeline_ && decoded->frames <= WindowFrames)
                timeline_.emplace(decoded->firstSample, sampleRate_, arrivalBudget_);
            admission = timeline_
                ? timeline_->reorder.insert(decoded->firstSample, std::span(scratch_).first(decoded->frames))
                : PacketAdmission::outsideWindow;
        }
        switch (admission) {
        case PacketAdmission::accepted: acceptedPackets_.add(1); break;
        case PacketAdmission::invalid: invalidPackets_.add(1); break;
        case PacketAdmission::late: latePackets_.add(1); break;
        case PacketAdmission::outsideWindow: outsideWindowPackets_.add(1); break;
        case PacketAdmission::overlap: overlappingPackets_.add(1); break;
        }
        return admission;
    }

    // Transport thread. Call after receiving packets and periodically during gaps.
    // In first-packet mode, elapsed starts at the arrival of the first admitted packet.
    ReorderDrain service(std::chrono::nanoseconds elapsed) noexcept {
        if (!timeline_) return {};
        const auto drained = timeline_->reorder.pump(playback_, scratch_, timeline_->clock.concealBefore(elapsed));
        concealedSourceFrames_.add(drained.missing);
        return drained;
    }

    // Transport thread. Snapshot of the playback queue, excluding interpolation lookahead.
    std::size_t queuedPlaybackFrames() const noexcept {
        return PlaybackFrames - playback_.writableFrames();
    }

    // Audio callback. Return the number of output frames derived from the queue.
    std::size_t render(std::span<Frame<Channels>> output) noexcept {
        const auto rendered = playback_.render(output);
        unfilledOutputFrames_.add(output.size() - rendered);
        return rendered;
    }

    // Read from any thread. Counters are cumulative and sampled independently.
    ReceiverStats stats() const noexcept {
        return {acceptedPackets_.get(), invalidPackets_.get(), latePackets_.get(),
                outsideWindowPackets_.get(), overlappingPackets_.get(),
                concealedSourceFrames_.get(), unfilledOutputFrames_.get()};
    }

private:
    // Each counter has exactly one writer. Readers only need the numeric value.
    struct Counter {
        void add(std::size_t increment) noexcept {
            if (increment) value.store(get() + increment, std::memory_order_relaxed);
        }
        std::size_t get() const noexcept { return value.load(std::memory_order_relaxed); }
        std::atomic<std::size_t> value{};
    };

    struct Timeline {
        Timeline(std::uint64_t firstSample, std::uint32_t rate, std::chrono::nanoseconds budget) noexcept
            : reorder(firstSample), clock(firstSample, rate, budget) {}

        ReorderBuffer<Channels, WindowFrames> reorder;
        PlayoutClock clock;
    };

    const std::uint32_t sampleRate_;
    const std::chrono::nanoseconds arrivalBudget_;
    std::optional<Timeline> timeline_;
    ReceiveBuffer<Channels, PlaybackFrames> playback_;
    std::array<Frame<Channels>, (maxPacketBytes - detail::headerBytes) / sizeof(float) / Channels> scratch_{};
    Counter acceptedPackets_, invalidPackets_, latePackets_, outsideWindowPackets_,
        overlappingPackets_, concealedSourceFrames_;
    alignas(64) Counter unfilledOutputFrames_;
};

} // namespace cell::network_audio
