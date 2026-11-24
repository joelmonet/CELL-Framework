#pragma once
#include <cell/core.hpp>
#include <memory>
#include <limits>
#include <cassert>

namespace cell {

// One non-audio producer owns allocation/reclamation; one audio consumer owns
// the active mutable DSP object. request() may also be called by a GUI thread.
// An audio borrow lasts until the next audioUpdate(). Destruction/shutdown must
// happen after both threads stop. No shared_ptr, mutex, allocation or deletion
// occurs in audioUpdate(). Published objects must already be fully prepared.
template<class T, std::size_t Slots = 4>
class PreparedExchange {
    static_assert(Slots >= 2 && Slots <= 256);
    static_assert(std::atomic<std::uint64_t>::is_always_lock_free);
    struct Token { std::uint32_t slot{}; std::uint64_t generation{}; };
public:
    using Ticket = std::uint64_t;
    PreparedExchange() = default;
    PreparedExchange(const PreparedExchange&) = delete;
    PreparedExchange& operator=(const PreparedExchange&) = delete;

    Ticket request() noexcept {
        auto value = requested_.load(std::memory_order_relaxed);
        do { if (value == std::numeric_limits<Ticket>::max()) return 0; }
        while (!requested_.compare_exchange_weak(value, value + 1, std::memory_order_acq_rel, std::memory_order_relaxed));
        return value + 1;
    }
    // Producer only. Failure retains the caller's ownership. The generation is
    // rechecked on audio: a newer request can race this queue publication.
    bool publish(Ticket ticket, std::unique_ptr<T>& prepared) noexcept {
        if (!prepared || !ticket || ticket != requested_.load(std::memory_order_acquire)) return false;
        reclaim();
        for (std::size_t n = 0; n < Slots; ++n) if (!objects_[n]) {
            objects_[n] = std::move(prepared);
            if (ready_.push({std::uint32_t(n), ticket})) return true;
            prepared = std::move(objects_[n]); return false;
        }
        return false;
    }
    // Producer-only transaction for immediately prepared replacements. Reserve
    // capacity before changing the generation: failed backpressure must not
    // cancel the last successfully accepted state. commit() runs before the
    // release publication and must be bounded, non-throwing and non-allocating.
    // Its caller serializes producers; no external cancellation is used by a
    // product that commits related control metadata here.
    template<class Commit>
    Ticket publishLatest(std::unique_ptr<T>& prepared,Commit commit)noexcept {
        static_assert(std::is_nothrow_invocable_v<Commit,T&,Ticket>);
        if(!prepared)return 0;
        reclaim();
        if(!ready_.freeCapacity())return 0;
        for(std::size_t n=0;n<Slots;++n)if(!objects_[n]){
            const auto ticket=request();if(!ticket)return 0;
            objects_[n]=std::move(prepared);commit(*objects_[n],ticket);
            // Only this producer pushes ready_; the consumer can only free it.
            const bool queued=ready_.push({std::uint32_t(n),ticket});
            assert(queued);(void)queued;return ticket;
        }
        return 0;
    }
    // Producer only. Retired objects are never destroyed by the audio thread.
    std::size_t reclaim() noexcept {
        Token token; std::size_t count{};
        while (count < Slots && retired_.pop(token)) { objects_[token.slot].reset(); ++count; }
        return count;
    }
    // Audio only, between uses of the previous borrow. Coalesce bounded ready
    // work to the newest current request. Under retirement backpressure leave
    // ready work queued and keep the current valid object.
    T* audioUpdate() noexcept {
        if (previous_.slot != invalid) return audioCurrent();
        for (std::size_t n = 0; n < Slots && retired_.freeCapacity(); ++n) {
            Token next; if (!ready_.pop(next)) break;
            if (next.generation != requested_.load(std::memory_order_acquire)) {
                const bool ok = retired_.push(next); (void)ok; continue;
            }
            if (active_.slot != invalid) { const bool ok = retired_.push(active_); (void)ok; }
            active_ = next;
            activeGeneration_.store(next.generation, std::memory_order_release);
        }
        return active_.slot == invalid ? nullptr : objects_[active_.slot].get();
    }
    T* audioCurrent() noexcept { return active_.slot == invalid ? nullptr : objects_[active_.slot].get(); }
    // Optional audio handoff for crossfading stateful DSP. Keep the previous
    // object borrowed until audioReleasePrevious() succeeds. While it is held,
    // further updates remain queued; the producer cannot reclaim its storage.
    // The caller advances both DSP objects, then releases between sample uses.
    T* audioUpdateRetainingPrevious() noexcept {
        if (previous_.slot != invalid) return audioCurrent();
        for (std::size_t n = 0; n < Slots && retired_.freeCapacity(); ++n) {
            Token next; if (!ready_.pop(next)) break;
            if (next.generation != requested_.load(std::memory_order_acquire)) {
                const bool ok = retired_.push(next); (void)ok; continue;
            }
            previous_ = active_;active_ = next;
            activeGeneration_.store(next.generation,std::memory_order_release);
            break;
        }
        return audioCurrent();
    }
    T* audioPrevious() noexcept { return previous_.slot == invalid ? nullptr : objects_[previous_.slot].get(); }
    bool audioReleasePrevious() noexcept {
        if (previous_.slot == invalid) return true;
        if (!retired_.freeCapacity()) return false;
        const bool ok=retired_.push(previous_);assert(ok);(void)ok;
        previous_={invalid,0};return true;
    }
    Ticket activeGeneration() const noexcept { return activeGeneration_.load(std::memory_order_acquire); }
    Ticket requestedGeneration() const noexcept { return requested_.load(std::memory_order_acquire); }
private:
    static constexpr auto invalid = std::numeric_limits<std::uint32_t>::max();
    std::array<std::unique_ptr<T>, Slots> objects_{};
    SpscQueue<Token, Slots + 1> ready_, retired_;
    Token active_{invalid, 0}; // audio thread only
    Token previous_{invalid, 0}; // optional audio-owned transition borrow
    alignas(64) std::atomic<Ticket> requested_{};
    alignas(64) std::atomic<Ticket> activeGeneration_{};
};
}
