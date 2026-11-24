// Original Cell Framework implementation; see LICENSE.
#pragma once
#include <cell/midi.hpp>
namespace cell::midi {
class Delivery final:public Output {
public:
    void reset() noexcept {
        if (sent_ || umpGroups_[0] || umpGroups_[1]) pending_ = true;
        legacyCleanup_ |= sent_;
        cursor_ = 0;
    }
    void begin(Output* destination,std::uint32_t frames)noexcept{
        destination_=destination;frames_=frames;last_=0;blocked_=false;sysEx_.clear();
        if(!pending_)return;
        if(!destination_){blocked_=true;return;}
        constexpr unsigned controls[]{64,120,123};
        while (cursor_ < 48) {
            const MidiEvent event{0, {std::uint8_t(0xb0 | (cursor_ / 3)), std::uint8_t(controls[cursor_ % 3]), 0}, 3};
            if (legacyCleanup_ && !destination_->push(event)) { blocked_ = true; return; }
            ++cursor_;
        }
        while (cursor_ < 48 + 32 * 48) {
            const auto slot = cursor_ - 48;
            const auto groupIndex = slot / 48;
            const auto dialect = groupIndex / 16, group = groupIndex % 16;
            if (!(umpGroups_[dialect] & (1u << group))) { cursor_ += 48; continue; }
            if (!destination_->push(umpAllNotesControl(dialect, group, (slot % 48) / 3, controls[slot % 3]))) {
                blocked_ = true;
                return;
            }
            ++cursor_;
        }
        pending_=sent_=false;cursor_=0;
        legacyCleanup_ = false;
        umpGroups_ = {};
    }
    bool push(MidiEvent event)noexcept override{
        if(blocked_)return false;
        if (event.isUmp()) {
            if (!validUmp(event) || event.frame >= frames_ || event.frame < last_) return false;
            const auto type = event.ump[0] >> 28;
            const bool channelVoice = type == 2 || type == 4;
            if (!destination_ || !destination_->push(event)) {
                if (channelVoice && (umpGroups_[0] || umpGroups_[1])) reject();
                return false;
            }
            if (channelVoice) umpGroups_[type == 4] |= std::uint16_t(1u << ((event.ump[0] >> 24) & 15));
            last_ = event.frame;
            return true;
        }
        if(event.isSysEx()){
            if(event.frame>=frames_||event.frame<last_||!destination_||
               !sysEx_.admit(event.sysEx)||!destination_->push(event))return false;
            last_=event.frame;return true;
        }
        if(!valid(event)||event.frame>=frames_||event.frame<last_||!destination_||!destination_->push(event)){reject();return false;}
        last_=event.frame;sent_=true;return true;
    }
    void reject()noexcept override{
        pending_=blocked_=true;cursor_=0;
        legacyCleanup_ |= sent_ || !(umpGroups_[0] || umpGroups_[1]);
        if(destination_)destination_->reject();
    }
    bool pending()const noexcept{return pending_;}
private:
    Output* destination_{};SysExBudget sysEx_;std::uint32_t frames_{},last_{};unsigned cursor_{};
    std::array<std::uint16_t, 2> umpGroups_{};
    bool sent_{},pending_{},blocked_{},legacyCleanup_{};
};
}
