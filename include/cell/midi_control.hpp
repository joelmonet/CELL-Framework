#pragma once
#include <cell/core.hpp>
#include <cell/midi.hpp>

namespace cell::midi {
enum class ControlAction:std::uint32_t {open,event,releaseAll,close};
struct ControlMessage {
    std::uint64_t owner{},sequence{};
    ControlAction action=ControlAction::event;
    MidiEvent event;
};
// One serialized non-audio producer and one audio consumer. UI notes belong
// to a monotonically identified view, not to a saved preset. A stale close or
// delayed note from an older view cannot take over the current controller.
class ControlInput {
public:
    static constexpr unsigned queueCapacity=256,maximumVoices=128;
    static constexpr unsigned eventCapacity=queueCapacity*2+maximumVoices;
    bool submit(const ControlMessage& message)noexcept {
        if(!message.owner||!message.sequence)return false;
        if(message.action==ControlAction::open){
            if(message.owner<=owner_)return false;
            releaseAll();owner_=message.owner;sequence_=message.sequence;closed_=false;return true;
        }
        if(message.action!=ControlAction::event&&message.action!=ControlAction::releaseAll&&message.action!=ControlAction::close)return false;
        const auto status=message.event.data[0]&0xf0;
        if(message.action==ControlAction::event&&(!valid(message.event)||(status!=0x80&&status!=0x90)))return false;
        // Component messages can reach a freshly prepared graph after the
        // view's initial open. A newer owner can establish its lease with an
        // event or release; a close records a tombstone even without an open.
        if(message.owner>owner_){releaseAll();owner_=message.owner;sequence_=0;closed_=false;}
        if(closed_||message.owner!=owner_||message.sequence<=sequence_)return false;
        if(message.action==ControlAction::releaseAll||message.action==ControlAction::close){
            sequence_=message.sequence;closed_=message.action==ControlAction::close;releaseAll();return true;
        }
        auto event=message.event;event.frame=0;
        sequence_=message.sequence;
        if(queue_.push({epoch_.load(std::memory_order_relaxed),event}))return true;
        // This release is independent of queue capacity. Closing a busy view
        // can never lose its final note-offs behind queued note-ons.
        releaseAll();return false;
    }
    void releaseAll()noexcept {epoch_.fetch_add(1,std::memory_order_release);}
    std::span<const MidiEvent> drain()noexcept {
        count_=0;Packet packet;
        synchronize(epoch_.load(std::memory_order_acquire));
        unsigned consumed=0;
        while(consumed++<queueCapacity&&queue_.pop(packet)){
            const auto epoch=epoch_.load(std::memory_order_acquire);synchronize(epoch);
            if(packet.epoch!=epoch)continue;
            const auto& event=packet.event;const auto key=unsigned(event.data[0]&15)*128+event.data[1];
            const bool on=(event.data[0]&0xf0)==0x90&&event.data[2]!=0;
            if(on&&!active_[key]&&voices_==maximumVoices)continue;
            if(on){if(!active_[key]){active_[key]=true;++voices_;}append(event);}
            else if(active_[key]){active_[key]=false;--voices_;append(event);}
        }
        synchronize(epoch_.load(std::memory_order_acquire));return {events_.data(),count_};
    }
private:
    struct Packet {std::uint64_t epoch{};MidiEvent event;};
    void append(MidiEvent event)noexcept {if(count_<events_.size())events_[count_++]=event;}
    void synchronize(std::uint64_t epoch)noexcept {
        if(epoch==audioEpoch_)return;
        if(voices_)for(unsigned key=0;key<active_.size();++key)if(active_[key]){
            append({0,{std::uint8_t(0x80|(key/128)),std::uint8_t(key%128),0},3});active_[key]=false;
        }
        voices_=0;audioEpoch_=epoch;
    }
    SpscQueue<Packet,queueCapacity+1> queue_;
    std::atomic<std::uint64_t> epoch_{1};
    std::uint64_t owner_{},sequence_{},audioEpoch_{};bool closed_=true;
    std::array<bool,16*128> active_{};unsigned voices_{},count_{};
    std::array<MidiEvent,eventCapacity> events_{};
};
}
