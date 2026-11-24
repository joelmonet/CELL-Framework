#pragma once

#include <cell/parameters.hpp>
#include <cassert>
#include <memory>
#include <optional>
#include <limits>

namespace cell::wrapper {

// A format can deliver a small sequence token through its timed coefficient
// port, while the owning model retains immutable, bounded parameter packets.
// One serialized non-audio producer and one audio consumer. The producer must
// outlive the consumer and join it before destroying this object.
class ParameterTimeline {
public:
    static_assert(std::atomic<std::uint64_t>::is_always_lock_free);
    static constexpr std::size_t packetValues = 4096, packetCount = 64;
    using Value = std::pair<ParamId, double>;
    ParameterTimeline() = default;
    ParameterTimeline(const ParameterTimeline&) = delete;
    ParameterTimeline& operator=(const ParameterTimeline&) = delete;
    ~ParameterTimeline() {
        reclaim();
        delete pending_;
        Packet* packet{}; while (incoming_.pop(packet)) delete packet;
    }

    // Non-audio. Admission is all-or-nothing, including a whole-catalog update
    // split across multiple packets. A rejected batch remains the producer's
    // responsibility; it must retain/retry those values.
    std::optional<std::uint64_t> publish(const ParameterBank& bank, std::span<const Value> values) noexcept {
        reclaim();
        if(const auto* owner=bank_.load(std::memory_order_acquire);owner&&owner!=&bank)return {};
        if (values.empty()) return published_.load(std::memory_order_acquire);
        const auto count = values.size() / packetValues + (values.size() % packetValues != 0);
        if (count > packetCount || count > packetCount - outstanding_ ||
            sequence_ > std::numeric_limits<std::uint64_t>::max() - count) return {};
        for (const auto& [id, value] : values) if (!std::isfinite(value) || !bank.find(id)) return {};
        std::array<std::unique_ptr<Packet>, packetCount> owned;
        std::array<Packet*, packetCount> pointers{};
        try {
            for (std::size_t index = 0; index < count; ++index) {
                auto packet = std::make_unique<Packet>();
                packet->sequence = sequence_ + index + 1;
                const auto part = values.subspan(index * packetValues, std::min(packetValues, values.size() - index * packetValues));
                packet->values.assign(part.begin(), part.end());
                pointers[index] = packet.get(); owned[index] = std::move(packet);
            }
        } catch (...) { return {}; }
        if (!incoming_.pushBatch({pointers.data(), count})) return {};
        for (std::size_t index = 0; index < count; ++index) owned[index].release();
        outstanding_ += count; sequence_ += count;
        bank_.store(&bank,std::memory_order_release);
        issued_[sequence_%packetCount].store(sequence_,std::memory_order_release);
        published_.store(sequence_, std::memory_order_release);
        return sequence_;
    }

    // Audio. Only apply packets covered by the host's delivered token. Skipped
    // intermediate tokens still apply every preceding packet in order. Future
    // automation remains queued until its token reaches the render callback.
    bool applyThrough(std::uint64_t token, ParameterBank& bank) noexcept {
        if(const auto* owner=bank_.load(std::memory_order_acquire);owner&&owner!=&bank)return false;
        if (token < applied_ || token > published_.load(std::memory_order_acquire)) return false;
        if (token == applied_) return true;
        // Only a committed batch's final token is public. A forged intermediate
        // packet number must not expose half of a whole-catalog update.
        if(issued_[token%packetCount].load(std::memory_order_acquire)!=token)return false;
        while (applied_ < token) {
            if (!pending_ && !incoming_.pop(pending_)) return false;
            if (pending_->sequence != applied_ + 1 || pending_->sequence > token) return false;
            if (pending_->sequence > discardThrough_.load(std::memory_order_acquire))
                for (const auto& [id, value] : pending_->values) bank.set(id, value);
            applied_ = pending_->sequence;
            Packet* retired = pending_; pending_ = nullptr;
            // outstanding_ limits all queued/current/retired packets together,
            // so this retirement queue always has room for the current packet.
            const bool accepted = retired_.push(retired); assert(accepted); (void)accepted;
            appliedToken_.store(applied_, std::memory_order_release);
        }
        return true;
    }

    void reclaim() noexcept {
        Packet* packet{};
        while (retired_.pop(packet)) { delete packet; --outstanding_; }
    }
    std::uint64_t published() const noexcept { return published_.load(std::memory_order_acquire); }
    std::uint64_t applied() const noexcept { return appliedToken_.load(std::memory_order_acquire); }
    // Non-audio, with the render consumer excluded: restored product state
    // supersedes older coefficient packets, including tokens still in the host.
    void discardPublished() noexcept { discardThrough_.store(published(), std::memory_order_release); }
private:
    struct Packet { std::uint64_t sequence{}; std::vector<Value> values; };
    SpscQueue<Packet*, packetCount + 1> incoming_, retired_;
    std::atomic<std::uint64_t> published_{};
    std::atomic<std::uint64_t> discardThrough_{};
    std::atomic<std::uint64_t> appliedToken_{};
    std::atomic<const ParameterBank*> bank_{};
    // At most packetCount packets can be outstanding, so a newer issued token
    // cannot replace an unconsumed token at the same modulo index.
    std::array<std::atomic<std::uint64_t>,packetCount> issued_{};
    std::uint64_t sequence_{}; // producer
    std::size_t outstanding_{}; // producer, includes retired but unreclaimed data
    Packet* pending_{}; // audio
    std::uint64_t applied_{}; // audio
};

} // namespace cell::wrapper
