#pragma once
#include <cell/core.hpp>
#include <map>
#include <memory>
#include <cassert>
#include <stdexcept>

namespace cell {
// One serialized control producer, one audio consumer. Publications that have
// not reached audio are coalesced immediately on the producer. A long burst of
// registrations before the first callback cannot exhaust a pending-slot bank.
// The producer owns every object; audio only borrows until its next update.
// Both threads must stop before destruction. reclaim()/publish() are non-audio.
template<class T>
class LatestExchange {
    static_assert(std::atomic<T*>::is_always_lock_free);
public:
    LatestExchange()=default;
    LatestExchange(const LatestExchange&)=delete;
    LatestExchange& operator=(const LatestExchange&)=delete;
    bool publish(std::unique_ptr<T>& candidate) {
        if(!candidate)return false;
        reclaim();auto* next=candidate.get();
        // Allocation can throw before ownership moves or publication changes.
        auto [entry,inserted]=owned_.try_emplace(next,nullptr);
        if(!inserted)throw std::logic_error("Prepared object already owned");
        entry->second=std::move(candidate);
        // Atomic exchange grants exclusive ownership of an unconsumed pointer
        // to either the replacing producer or the audio consumer, never both.
        if(auto* superseded=pending_.exchange(next,std::memory_order_acq_rel))owned_.erase(superseded);
        return true;
    }
    T* audioUpdate() noexcept {
        if(!retired_.freeCapacity())return current_;
        if(auto* next=pending_.exchange(nullptr,std::memory_order_acq_rel)){
            if(current_){const bool queued=retired_.push(current_);assert(queued);(void)queued;}
            current_=next;
        }
        return current_;
    }
    T* audioCurrent() noexcept {return current_;}
    std::size_t reclaim() noexcept {
        T* retired{};std::size_t count{};
        while(retired_.pop(retired)){owned_.erase(retired);++count;}return count;
    }
private:
    std::map<T*,std::unique_ptr<T>> owned_; // producer only
    alignas(64)std::atomic<T*> pending_{};
    T* current_{}; // consumer only
    SpscQueue<T*,4> retired_;
};
}
