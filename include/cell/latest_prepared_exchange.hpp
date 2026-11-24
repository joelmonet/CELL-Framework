#pragma once
#include <cell/core.hpp>
#include <cassert>
#include <limits>
#include <memory>

namespace cell {
// One serialized control producer, one audio consumer. A complete pending
// replacement may be superseded before audio takes ownership. This keeps
// stopped/suspended hosts editable without growing an unbounded DSP queue.
// Audio owns its current borrow until its next audioUpdate(). Only the
// producer destroys superseded or retired objects; shutdown follows both
// threads stopping. A retained-previous handoff supports short crossfades;
// the audio caller must release that borrow when the transition ends. Use
// PreparedExchange for explicit request cancellation.
template<class T, std::size_t Slots = 3>
class LatestPreparedExchange {
    static_assert(Slots >= 2 && Slots <= 256);
    static_assert(std::atomic<std::uint32_t>::is_always_lock_free);
    static constexpr std::uint32_t invalid = Slots;
public:
    using Ticket = std::uint64_t;
    LatestPreparedExchange() = default;
    LatestPreparedExchange(const LatestPreparedExchange&) = delete;
    LatestPreparedExchange& operator=(const LatestPreparedExchange&) = delete;

    template<class Commit>
    Ticket publishLatest(std::unique_ptr<T>& prepared, Commit commit) noexcept {
        static_assert(std::is_nothrow_invocable_v<Commit,T&,Ticket>);
        if(!prepared || generation_ == std::numeric_limits<Ticket>::max()) return 0;
        reclaim();
        auto slot = freeSlot();
        if(slot == invalid) {
            // Atomic ownership transfer: a candidate already claimed by audio
            // cannot also be removed here. Reusing an unclaimed slot is safe
            // even when callbacks have stopped indefinitely.
            slot = pending_.exchange(invalid, std::memory_order_acq_rel);
            if(slot == invalid) { reclaim(); slot = freeSlot(); }
            if(slot == invalid) return 0;
        }
        objects_[slot] = std::move(prepared);
        const auto ticket = ++generation_;
        commit(*objects_[slot], ticket);
        const auto previous = pending_.exchange(slot, std::memory_order_acq_rel);
        if(previous != invalid) objects_[previous].reset();
        return ticket;
    }
    std::size_t reclaim() noexcept {
        std::uint32_t slot; std::size_t count{};
        while(count < Slots && retired_.pop(slot)) { objects_[slot].reset(); ++count; }
        return count;
    }
    T* audioUpdate() noexcept {
        if(previous_ != invalid) return audioCurrent();
        if(!retired_.freeCapacity()) return audioCurrent();
        const auto next = pending_.exchange(invalid, std::memory_order_acq_rel);
        if(next != invalid) {
            if(active_ != invalid) { const bool ok = retired_.push(active_); assert(ok); (void)ok; }
            active_ = next;
        }
        return audioCurrent();
    }
    // Audio only. While a previous borrow is held, newer producer updates
    // continue coalescing into pending_; adoption waits until release. Use
    // at least three slots for simultaneous previous/current/pending storage.
    T* audioUpdateRetainingPrevious() noexcept {
        if(previous_ != invalid || !retired_.freeCapacity()) return audioCurrent();
        const auto next = pending_.exchange(invalid, std::memory_order_acq_rel);
        if(next != invalid) { previous_ = active_; active_ = next; }
        return audioCurrent();
    }
    T* audioPrevious() noexcept { return previous_ == invalid ? nullptr : objects_[previous_].get(); }
    bool audioReleasePrevious() noexcept {
        if(previous_ == invalid) return true;
        if(!retired_.push(previous_)) return false;
        previous_ = invalid; return true;
    }
    T* audioCurrent() noexcept { return active_ == invalid ? nullptr : objects_[active_].get(); }
private:
    std::uint32_t freeSlot() const noexcept {
        for(std::uint32_t n = 0; n < Slots; ++n) if(!objects_[n]) return n;
        return invalid;
    }
    std::array<std::unique_ptr<T>,Slots> objects_{};
    Ticket generation_{}; // producer only
    std::uint32_t active_ = invalid, previous_ = invalid; // audio only
    alignas(64) std::atomic<std::uint32_t> pending_{invalid};
    SpscQueue<std::uint32_t,Slots+1> retired_;
};
}
