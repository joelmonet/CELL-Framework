// SPDX-License-Identifier: Apache-2.0
#include <cell/midi_tools.hpp>
#include <cmath>
#include <limits>

namespace cell::midi {
Tools::Tools()noexcept{clear();}
void Tools::clear()noexcept{
    heads_.fill(none);tails_.fill(none);owners_.fill(0);sustain_.fill(0);sustainDown_.fill(false);
    for(auto& row:channelOwners_)row.fill(0);
    for(std::size_t n=0;n<voices_.size();++n){voices_[n].next=n+1<voices_.size()?std::uint16_t(n+1):none;voices_[n].count=0;}
    free_=0;stats_.held=stats_.sounding=0;
}
bool Tools::configure(const ToolsConfig& c)noexcept{
    if(c.inputChannel>16||c.outputChannel>16||c.transpose< -127||c.transpose>127||c.keyLow>c.keyHigh||c.keyHigh>127||c.velocityLow<1||c.velocityLow>c.velocityHigh||c.velocityHigh>127||c.outputVelocityLow<1||c.outputVelocityLow>c.outputVelocityHigh||c.outputVelocityHigh>127||!std::isfinite(c.velocityScale)||c.velocityScale<0||c.velocityScale>4||!std::isfinite(c.velocityCurve)||c.velocityCurve<.125||c.velocityCurve>8||c.root>11||!c.scale||c.scale>0xfff||!c.chordVoices||c.chordVoices>8)return false;
    for(unsigned ch:c.channels)if(ch>15)return false;
    for(int note:c.chord)if(note< -127||note>127)return false;
    for(const auto& cc:c.cc)if(cc.source>128||cc.target>127||cc.minimum>cc.maximum||cc.maximum>127)return false;
    const bool gateChanged=c.bypass!=config_.bypass||c.notes!=config_.notes||c.controllers!=config_.controllers;
    const bool controllersChanged=c.cc!=config_.cc;
    if(gateChanged||controllersChanged){
        const bool pedalActive=std::any_of(sustain_.begin(),sustain_.end(),[](auto x){return x!=0;});
        if((gateChanged&&(stats_.held||pedalActive))||(controllersChanged&&pedalActive))panic_=true;
    }
    config_=c;return true;
}
void Tools::reset()noexcept{panic_=panic_||stats_.held||std::any_of(sustain_.begin(),sustain_.end(),[](auto x){return x!=0;});playing_=clock_=false;}
bool Tools::send(Output& output,std::uint32_t frame,unsigned status,unsigned a,unsigned b,unsigned size)noexcept{
    if(!output.push({frame,{std::uint8_t(status),std::uint8_t(a),std::uint8_t(b)},std::uint8_t(size)}))return false;
    ++stats_.output;return true;
}
bool Tools::panic(Output& output,std::uint32_t frame)noexcept{
    bool result=true;
    for(unsigned ch=0;ch<16;++ch)for(unsigned cc:{64u,120u,123u})result=send(output,frame,0xb0|ch,cc,0)&&result;
    clear();panic_=!result;return result;
}
unsigned Tools::destination(unsigned ch)const noexcept{return config_.bypass?ch:config_.outputChannel?config_.outputChannel-1:config_.channels[ch];}
std::uint16_t Tools::destinations(unsigned ch)const noexcept{
    auto mask=std::uint16_t(1u<<destination(ch));
    for(unsigned out=0;out<16;++out)if(channelOwners_[ch][out])mask|=std::uint16_t(1u<<out);
    return mask;
}
int Tools::pitch(int note)const noexcept{
    if(note<0||note>127)return -1;
    const auto allowed=[&](int value){return value>=0&&value<=127&&(config_.scale&(1u<<((value-int(config_.root)+120)%12)));};
    if(allowed(note))return note;
    for(int d=1;d<12;++d){if(allowed(note-d))return note-d;if(allowed(note+d))return note+d;}
    return -1;
}
bool Tools::noteOn(const MidiEvent& event,Output& output)noexcept{
    const unsigned ch=event.data[0]&15,key=ch*128+event.data[1],velocity=event.data[2];
    const bool filtered=!config_.bypass&&(!config_.notes||(config_.inputChannel&&config_.inputChannel!=ch+1)||event.data[1]<config_.keyLow||event.data[1]>config_.keyHigh||velocity<config_.velocityLow||velocity>config_.velocityHigh);
    if(free_==none){panic_=true;return false;}
    const int base=filtered?-1:config_.bypass?event.data[1]:pitch(int(event.data[1])+config_.transpose);
    std::array<std::uint16_t,8> destinations{};unsigned count{};
    if(base>=0)for(unsigned n=0;n<(config_.bypass?1:config_.chordVoices);++n){const int note=base+(config_.bypass?0:config_.chord[n]);if(note<0||note>127)continue;const auto out=std::uint16_t(destination(ch)*128+unsigned(note));if(std::find(destinations.begin(),destinations.begin()+count,out)==destinations.begin()+count)destinations[count++]=out;}
    // Silent ownership entries retain FIFO pairing for filtered/repeated keys.
    if(!count)++stats_.rejected;
    const auto index=free_;auto& voice=voices_[index];free_=voice.next;voice.next=none;voice.key=std::uint16_t(key);voice.count=std::uint8_t(count);voice.destinations=destinations;
    if(tails_[key]!=none)voices_[tails_[key]].next=index;else heads_[key]=index;tails_[key]=index;++stats_.held;
    if(!count)return true;
    const double curved=config_.velocityCurve==1?double(velocity):std::pow(double(velocity)/127,config_.velocityCurve)*127;
    const unsigned value=config_.bypass?velocity:unsigned(std::clamp(std::lround(curved*config_.velocityScale),long(config_.outputVelocityLow),long(config_.outputVelocityHigh)));
    for(unsigned n=0;n<count;++n){const auto out=destinations[n];++channelOwners_[ch][out/128];if(sustainDown_[ch]&&!(sustain_[ch]&(1u<<(out/128)))){sustain_[ch]|=std::uint16_t(1u<<(out/128));if(!send(output,event.frame,0xb0|(out/128),64,127))return false;}if(owners_[out]++==0){++stats_.sounding;if(!send(output,event.frame,0x90|(out/128),out%128,value))return false;}}
    return true;
}
bool Tools::noteOff(unsigned key,unsigned velocity,std::uint32_t frame,Output& output)noexcept{
    const auto index=heads_[key];if(index==none)return true;auto& voice=voices_[index];heads_[key]=voice.next;if(heads_[key]==none)tails_[key]=none;
    bool result=true;for(unsigned n=0;n<voice.count;++n){const auto out=voice.destinations[n];--channelOwners_[key/128][out/128];if(owners_[out]&&--owners_[out]==0){--stats_.sounding;result=send(output,frame,0x80|(out/128),out%128,velocity)&&result;}}
    voice.next=free_;free_=index;--stats_.held;return result;
}
bool Tools::releaseChannel(unsigned ch,std::uint32_t frame,Output& output)noexcept{
    bool result=true;for(unsigned key=ch*128;key<(ch+1)*128;++key)while(heads_[key]!=none)result=noteOff(key,0,frame,output)&&result;return result;
}
bool Tools::message(const MidiEvent& event,Output& output)noexcept{
    const unsigned status=event.data[0],kind=status&0xf0,ch=status&15,a=event.data[1],b=event.data[2];
    if(kind==0x80||(kind==0x90&&b==0))return noteOff(ch*128+a,b,event.frame,output);
    if(kind==0x90)return noteOn(event,output);
    if(kind==0xb0&&(a==120||a==123)){if(!releaseChannel(ch,event.frame,output))return false;if(a==123)return true;auto mask=sustain_[ch];sustain_[ch]=0;sustainDown_[ch]=false;for(unsigned out=0;out<16;++out)if(mask&(1u<<out))if(!send(output,event.frame,0xb0|out,64,0))return false;return true;}
    if(kind==0xb0&&(a==121||(a==64&&b<64&&config_.inputChannel&&config_.inputChannel!=ch+1))){
        auto mask=sustain_[ch];sustain_[ch]=0;sustainDown_[ch]=false;
        if((config_.bypass||config_.controllers)&&(!config_.inputChannel||config_.inputChannel==ch+1))mask|=destinations(ch);
        for(unsigned out=0;out<16;++out)if(mask&(1u<<out))if(!send(output,event.frame,0xb0|out,a,b))return false;return true;
    }
    if(!config_.bypass&&config_.inputChannel&&config_.inputChannel!=ch+1){++stats_.rejected;return true;}
    if(kind==0xa0){if(!config_.bypass&&!config_.pressure)return true;for(auto index=heads_[ch*128+a];index!=none;index=voices_[index].next)for(unsigned n=0;n<voices_[index].count;++n){const auto out=voices_[index].destinations[n];if(!send(output,event.frame,0xa0|(out/128),out%128,b))return false;}return true;}
    if(!config_.bypass&&((kind==0xb0&&!config_.controllers)||(kind==0xe0&&!config_.pitchBend)||(kind==0xd0&&!config_.pressure)||(kind==0xc0&&!config_.programs)))return true;
    if(kind==0xb0){
        std::array<std::pair<unsigned,unsigned>,5> emitted{};unsigned emittedCount{};
        auto emitController=[&](unsigned target,unsigned value)noexcept{
            const auto message=std::pair{target,value};
            if(std::find(emitted.begin(),emitted.begin()+emittedCount,message)!=emitted.begin()+emittedCount)return true;
            emitted[emittedCount++]=message;
            auto mask=destinations(ch);
            if(target==64){
                sustainDown_[ch]=value>=64;
                if(value>=64)sustain_[ch]|=mask;
                else {mask|=sustain_[ch];sustain_[ch]=0;}
            }
            for(unsigned out=0;out<16;++out)if(mask&(1u<<out))
                if(!send(output,event.frame,0xb0|out,target,value))return false;
            return true;
        };
        bool mapped=false,keep=false;
        if(!config_.bypass)for(const auto& cc:config_.cc)if(cc.source==a){
            mapped=true;keep|=cc.keepSource;
            const double t=cc.inverted?1-double(b)/127:double(b)/127;
            const unsigned value=unsigned(std::lround(cc.minimum+t*(cc.maximum-cc.minimum)));
            if(!emitController(cc.target,value))return false;
        }
        if((!mapped||keep)&&!emitController(a,b))return false;
        return true;
    }
    const auto mask=destinations(ch);
    for(unsigned out=0;out<16;++out)if(mask&(1u<<out))
        if(!send(output,event.frame,kind|out,a,b,kind==0xc0||kind==0xd0?2:3))return false;
    return true;
}
bool Tools::process(std::uint32_t frames,std::span<const MidiEvent> input,Output& output,const Transport& transport)noexcept{
    const bool discontinuity=(playing_&&!transport.playing)||(playing_&&transport.playing&&clock_&&transport.samplePositionValid&&transport.samplePosition!=expected_);
    if(discontinuity&&(stats_.held||std::any_of(sustain_.begin(),sustain_.end(),[](auto x){return x!=0;})))panic_=true;
    playing_=transport.playing;clock_=transport.samplePositionValid;expected_=transport.samplePosition>std::numeric_limits<std::int64_t>::max()-frames?std::numeric_limits<std::int64_t>::max():transport.samplePosition+frames;
    if(!frames)return true;
    if(panic_&&!panic(output,0))return false;
    std::uint32_t previous{};
    SysExBudget sysEx;
    bool accepted = true;
    for(const auto& event:input){++stats_.input;if(event.frame>=frames||event.frame<previous||!(event.isSysEx()?sysEx.admit(event.sysEx):valid(event))){++stats_.rejected;continue;}previous=event.frame;
        if(event.isSysEx()){
            if(output.push(event))++stats_.output;
            else{++stats_.overflows;accepted=false;}
            continue;
        }
        if(!message(event,output)){++stats_.overflows;panic_=true;return false;}}
    return accepted;
}
}
