// Original Cell Framework implementation; see LICENSE.
#pragma once
#include <cell/midi.hpp>
namespace cell::midi {
class Delivery final:public Output {
public:
    void reset()noexcept{if(sent_)pending_=true;cursor_=0;}
    void begin(Output* destination,std::uint32_t frames)noexcept{
        destination_=destination;frames_=frames;last_=0;blocked_=false;
        if(!pending_)return;
        if(!destination_){blocked_=true;return;}
        constexpr unsigned controls[]{64,120,123};
        while(cursor_<48){const MidiEvent e{0,{std::uint8_t(0xb0|(cursor_/3)),std::uint8_t(controls[cursor_%3]),0},3};if(!destination_->push(e)){blocked_=true;return;}++cursor_;}
        pending_=sent_=false;cursor_=0;
    }
    bool push(MidiEvent event)noexcept override{
        if(blocked_)return false;
        if(!valid(event)||event.frame>=frames_||event.frame<last_||!destination_||!destination_->push(event)){reject();return false;}
        last_=event.frame;sent_=true;return true;
    }
    void reject()noexcept override{pending_=blocked_=true;cursor_=0;if(destination_)destination_->reject();}
    bool pending()const noexcept{return pending_;}
private:
    Output* destination_{};std::uint32_t frames_{},last_{};unsigned cursor_{};bool sent_{},pending_{},blocked_{};
};
}
