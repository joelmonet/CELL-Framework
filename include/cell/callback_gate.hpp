#pragma once
#include <atomic>
#include <chrono>
#include <cstdint>
#include <thread>

namespace cell {
// One audio callback at a time. Control operations may pause callback entry
// while replacing stopped-only product state. Audio never waits or takes a lock;
// a paused adapter emits silence. This is not a seamless preset transition.
class CallbackGate {
public:
    class AudioScope {
    public:
        explicit AudioScope(CallbackGate& gate)noexcept:gate_(gate){std::uint32_t empty=0;active_=gate_.state_.compare_exchange_strong(empty,audio,std::memory_order_acquire);}
        ~AudioScope(){if(active_)gate_.state_.fetch_and(~audio,std::memory_order_release);}
        AudioScope(const AudioScope&)=delete;
        AudioScope& operator=(const AudioScope&)=delete;
        explicit operator bool()const noexcept{return active_;}
    private:CallbackGate& gate_;bool active_{};
    };
    class PauseScope {
    public:
        explicit PauseScope(CallbackGate& gate,bool required=true,std::chrono::milliseconds timeout=std::chrono::seconds(1))noexcept:gate_(gate){
            if(!required){accepted_=true;return;}
            if(gate_.state_.fetch_or(paused,std::memory_order_acq_rel)&paused)return;
            const auto deadline=std::chrono::steady_clock::now()+timeout;
            while(gate_.state_.load(std::memory_order_acquire)&audio){
                if(std::chrono::steady_clock::now()>=deadline){gate_.state_.fetch_and(~paused,std::memory_order_release);return;}
                std::this_thread::yield();
            }
            accepted_=owns_=true;
        }
        ~PauseScope(){if(owns_)gate_.state_.fetch_and(~paused,std::memory_order_release);}
        PauseScope(const PauseScope&)=delete;
        PauseScope& operator=(const PauseScope&)=delete;
        explicit operator bool()const noexcept{return accepted_;}
    private:CallbackGate& gate_;bool accepted_{},owns_{};
    };
private:
    static constexpr std::uint32_t audio=1,paused=2;
    static_assert(std::atomic<std::uint32_t>::is_always_lock_free);
    std::atomic<std::uint32_t> state_{};
};
}
