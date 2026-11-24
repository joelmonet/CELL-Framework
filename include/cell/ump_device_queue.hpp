// Original Cell Framework implementation; see LICENSE.
#pragma once
#include <cell/core.hpp>
#include <cell/midi.hpp>

namespace cell::midi {

// One device producer and one audio callback consumer. Android UMP ports use
// most-significant byte first within each word. A device packet contains only
// complete messages and shares one monotonic nanosecond timestamp.
class UmpDeviceQueue {
public:
    static constexpr std::size_t capacity = 1024;
    struct Block { std::size_t count{}; bool reset{}; };

    bool push(std::span<const std::uint8_t> bytes, std::int64_t timestampNs) noexcept {
        if (bytes.empty() || bytes.size() % 4 || bytes.size() > 1024 || timestampNs < 0) return false;
        const auto epoch = generation_.load(std::memory_order_relaxed);
        // Zero means immediate. Keep it behind any already queued device data.
        const auto time = timestampNs ? timestampNs : lastTimestamp_;
        if (time < lastTimestamp_) return false;
        std::array<Entry, 256> decoded;
        std::size_t count{};
        for (std::size_t offset = 0; offset < bytes.size();) {
            std::array<std::uint32_t, 4> words{wordAt(bytes, offset)};
            const auto length = umpWordCount(words[0]);
            if (!length || length > (bytes.size() - offset) / 4) return false;
            for (std::size_t i = 1; i < length; ++i) words[i] = wordAt(bytes, offset + 4 * i);
            decoded[count++] = {MidiEvent::universalPacket(0, std::span(words).first(length)), time, epoch};
            offset += 4 * length;
        }
        if (!fifo_.pushBatch(std::span(decoded).first(count))) {
            flush();
            return false;
        }
        lastTimestamp_ = time;
        return true;
    }

    // Device thread on disconnect or MIDI flush. Read reports reset once so the
    // caller can release voices before delivering new events after a discontinuity.
    void flush() noexcept {
        lastTimestamp_ = 0;
        generation_.fetch_add(1, std::memory_order_release);
    }

    Block read(std::int64_t blockStartNs, double rate, std::uint32_t frames,
               std::span<MidiEvent> output) noexcept {
        if (blockStartNs < 0 || !std::isfinite(rate) || rate <= 0 || !frames) return {};
        const auto epoch = generation_.load(std::memory_order_acquire);
        Block block{0, epoch != readGeneration_};
        readGeneration_ = epoch;
        // Snapshot availability so copying stays bounded even with a busy producer.
        auto remaining = fifo_.available();
        while (block.count < output.size()) {
            if (!pending_) {
                if (!remaining || !fifo_.pop(next_)) break;
                --remaining;
                pending_ = true;
            }
            if (next_.generation != epoch) {
                // A producer flush racing this read belongs to the next block.
                if (next_.generation > epoch) break;
                pending_ = false;
                continue;
            }
            const auto delta = next_.time > blockStartNs ? next_.time - blockStartNs : 0;
            const double offset = static_cast<double>(delta) * rate / 1e9;
            if (offset >= frames) break;
            next_.event.frame = static_cast<std::uint32_t>(offset);
            output[block.count++] = next_.event;
            pending_ = false;
        }
        return block;
    }

    static std::size_t encode(MidiEvent event, std::span<std::uint8_t> bytes) noexcept {
        if (!validUmp(event) || bytes.size() < 4u * event.umpCount) return 0;
        for (std::size_t i = 0; i < event.umpCount; ++i)
            for (unsigned byte = 0; byte < 4; ++byte)
                bytes[4 * i + byte] = static_cast<std::uint8_t>(event.ump[i] >> (24 - 8 * byte));
        return 4u * event.umpCount;
    }

private:
    static std::uint32_t wordAt(std::span<const std::uint8_t> bytes, std::size_t offset) noexcept {
        std::uint32_t word{};
        for (unsigned byte = 0; byte < 4; ++byte) word = (word << 8) | bytes[offset + byte];
        return word;
    }

    struct Entry { MidiEvent event; std::int64_t time; std::uint32_t generation; };
    SpscQueue<Entry, capacity + 1> fifo_;
    std::atomic<std::uint32_t> generation_{};
    std::int64_t lastTimestamp_{};
    Entry next_{};
    std::uint32_t readGeneration_{};
    bool pending_{};
};
} // namespace cell::midi
