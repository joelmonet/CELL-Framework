// SPDX-License-Identifier: Apache-2.0
#include <cell/midi_tools.hpp>
#include <cell/plugin.hpp>
#include "allocation_watch.hpp"
#include <iostream>
using namespace cell;
namespace {
void check(bool value,const char* why){if(!value)throw std::runtime_error(why);}
MidiEvent event(unsigned frame,unsigned status,unsigned a,unsigned b=0,unsigned size=3){return {frame,{std::uint8_t(status),std::uint8_t(a),std::uint8_t(b)},std::uint8_t(size)};}
struct Harness {
    midi::Tools tools;midi::Buffer output;midi::ToolsConfig config;Transport transport;
    Harness(){output.prepare();}
    std::span<const MidiEvent> run(std::span<const MidiEvent> input,unsigned frames=64){
        check(tools.configure(config),"Valid MIDI configuration rejected");output.clear();cell_test::audio=true;cell_test::allocations=cell_test::deletions=0;
        const auto ok=tools.process(frames,input,output,transport);cell_test::audio=false;
        check(ok,"MIDI processing failed");check(!cell_test::allocations&&!cell_test::deletions,"MIDI callback allocated/reclaimed memory");return output.events();
    }
    std::span<const MidiEvent> run(std::initializer_list<MidiEvent> input){return run(std::span<const MidiEvent>(input.begin(),input.size()));}
};
void transparency(){
    Harness h;const std::array input{event(0,0x92,60,99),event(3,0xb2,1,64),event(7,0xe2,0,64),event(8,0xd2,77,0,2),event(9,0xc2,14,0,2),event(17,0xa2,60,80),event(63,0x82,60,31)};
    const auto out=h.run(input);check(out.size()==input.size(),"Default MIDI path altered message count");
    for(unsigned n=0;n<input.size();++n)check(out[n].frame==input[n].frame&&out[n].data==input[n].data&&out[n].size==input[n].size,"Default MIDI path altered message/timestamp");
    check(h.tools.stats().held==0&&h.tools.stats().sounding==0,"Released note retained ownership");
}
void mappingOwnership(){
    Harness h;h.config.transpose=12;h.config.outputChannel=4;auto out=h.run({event(7,0x90,60,100)});check(out.size()==1&&out[0].data==event(7,0x93,72,100).data,"Transpose/channel mapping wrong");
    h.config.transpose=-12;h.config.outputChannel=6;out=h.run({event(10,0x90,60,80)});check(out.size()==1&&out[0].data==event(10,0x95,48,80).data,"Second mapping was not admitted");
    h.config.inputChannel=16;h.config.keyLow=100;out=h.run({event(11,0x80,60,44),event(41,0x90,60,0)});check(out.size()==2&&out[0].data==event(11,0x83,72,44).data&&out[1].data==event(41,0x85,48).data,"Held-note release used changed mapping/filter");
    check(h.tools.stats().held==0&&h.tools.stats().sounding==0,"Changed mapping left held notes");
}
void collisionsAndFilters(){
    Harness h;h.config.outputChannel=1;auto out=h.run({event(1,0x90,60,100),event(2,0x91,60,80)});check(out.size()==1&&h.tools.stats().sounding==1&&h.tools.stats().held==2,"Colliding destinations were double-triggered");
    out=h.run({event(4,0x80,60)});check(out.empty(),"First collision owner prematurely released note");out=h.run({event(5,0x81,60)});check(out.size()==1&&(out[0].data[0]&0xf0)==0x80,"Last collision owner did not release note");
    h.config.velocityLow=50;out=h.run({event(0,0x90,60,20),event(1,0x90,60,100),event(2,0x80,60)});check(out.size()==1&&(out[0].data[0]&0xf0)==0x90,"Filtered key lost its FIFO release position");
    out=h.run({event(3,0x80,60)});check(out.size()==1&&(out[0].data[0]&0xf0)==0x80,"Accepted repeated note lost its release");
    h.config.transpose=48;out=h.run({event(0,0x90,100,100),event(2,0x80,100)});check(out.empty(),"Out-of-range note was clamped to a boundary key");
}
void harmonyVelocityAndPressure(){
    Harness h;h.config.scale=0xab5;h.config.chord={0,4,7};h.config.chordVoices=3;h.config.velocityScale=.5;
    auto out=h.run({event(6,0x90,61,100)});check(out.size()==3&&out[0].data[1]==60&&out[1].data[1]==64&&out[2].data[1]==67&&out[0].data[2]==50,"Scale/chord/velocity conversion wrong");
    h.config.transpose=12;out=h.run({event(7,0xa0,61,75)});check(out.size()==3&&out[0].data[1]==60&&out[2].data[1]==67,"Poly pressure did not follow held chord");
    out=h.run({event(9,0x80,61,30)});check(out.size()==3&&h.tools.stats().sounding==0,"Chord did not release all voices");
}
void controllersAndPedal(){
    {Harness duplicate;duplicate.config.cc[0]={1,1,0,127,false,true};const auto out=duplicate.run({event(0,0xb0,1,100)});check(out.size()==1,"Identity CC map plus original forwarding emitted duplicates");}
    {Harness held;held.run({event(0,0x90,60,100)});held.config.cc[0].target=11;check(held.run({}).empty()&&held.tools.stats().held==1,"Editing a disabled CC map interrupted held notes");held.config.cc[0].source=1;check(held.run({}).empty()&&held.tools.stats().held==1,"Enabling a CC map interrupted unrelated held notes");held.run({event(0,0x80,60)});}
    Harness h;h.config.cc[0]={1,11,20,100,true,true};auto out=h.run({event(1,0xb0,1,127)});check(out.size()==2&&out[0].data==event(1,0xb0,11,20).data&&out[1].data==event(1,0xb0,1,127).data,"CC curve/range/invert/thru mapping wrong");
    h.config.cc[0].source=128;out=h.run({event(1,0xb0,64,127)});check(out.size()==1,"Sustain down not forwarded");h.config.outputChannel=3;
    out=h.run({event(2,0x90,60,100)});check(out.size()==2&&out[0].data==event(2,0xb2,64,127).data&&out[1].data[0]==0x92,"New destination lost an already-held sustain pedal");
    h.run({event(3,0x80,60)});h.config.inputChannel=16;out=h.run({event(4,0xb0,64,0)});check(out.size()==2&&out[0].data==event(4,0xb0,64).data&&out[1].data==event(4,0xb2,64).data,"Pedal up lost old destination after routing/filter change");
    h.config.inputChannel=0;out=h.run({event(1,0xd0,50,255,3),event(2,0xc0,7,255,3)});check(out.size()==2&&out[0].size==2&&out[1].size==2,"Packed two-byte MIDI messages were rejected/not normalized");
    h.config.cc[0]={1,64,0,127,false,false};out=h.run({event(0,0xb0,1,127)});check(out.size()==1&&out[0].data[1]==64&&out[0].data[2]==127,"Mapped sustain press failed");h.config.outputChannel=7;out=h.run({event(1,0xb0,1,0)});check(out.size()==2&&out[0].data[1]==64&&out[1].data[1]==64&&out[0].data[2]==0&&out[1].data[2]==0,"Mapped sustain release lost the old destination");
}
void safety(){
    Harness h;h.run({event(0,0x90,60,100)});h.config.bypass=true;auto out=h.run({event(17,0x90,61,90)});check(out.size()==49&&out[48].frame==17&&out[48].data[1]==61,"Bypass failed to clear transformed notes before passthrough");
    out=h.run({event(3,0x80,61)});check(out.size()==1&&h.tools.stats().held==0,"Bypassed notes did not release");
    h.config.bypass=false;h.run({event(0,0x90,62,100)});h.tools.requestPanic();out=h.run({});check(out.size()==48&&h.tools.stats().held==0,"Panic left notes active");
    h.transport={.playing=true,.samplePosition=0,.samplePositionValid=true};h.run({event(1,0x90,63,100)});h.transport.samplePosition=4096;out=h.run({});check(out.size()==48&&h.tools.stats().held==0,"Transport seek left owned notes");
    h.run({event(2,0x90,64,100)});h.transport.playing=false;out=h.run({});check(out.size()==48,"Transport stop did not clear generated notes");
    auto bad=h.config;bad.keyLow=128;check(!h.tools.configure(bad),"Invalid note range accepted");bad=h.config;bad.velocityCurve=std::numeric_limits<double>::quiet_NaN();check(!h.tools.configure(bad),"NaN curve accepted");
    out=h.run({event(64,0x90,60,90),event(0,0x70,60,90),event(1,0x90,128,90),event(4,0x90,60,90,2)});check(out.empty(),"Malformed/out-of-block MIDI was forwarded");
    h.transport.samplePosition=std::numeric_limits<std::int64_t>::max();h.transport.samplePositionValid=true;h.run({});
}
void overflow(){
    midi::Tools tools;midi::ToolsConfig config;config.chord={0,3,7,10};config.chordVoices=4;check(tools.configure(config),"Chord config rejected");midi::Buffer output;output.prepare(48);std::array<MidiEvent,32> input{};for(unsigned n=0;n<input.size();++n)input[n]=event(n,0x90|(n%16),40+(n/16)*24,100);
    check(!tools.process(64,input,output)&&output.overflowed(),"Overflow was silently accepted");output.emergency();check(output.events().size()==48&&output.overflowed(),"Overflow did not retain a complete panic batch");output.clear();check(tools.process(64,{},output)&&tools.stats().held==0&&output.events().size()==48,"Overflow left abandoned note ownership");
}
void poolAndRandomizedEvents(){
    midi::Tools tools;midi::Buffer output;output.prepare();std::vector<MidiEvent> full(midi::Tools::voices);
    for(unsigned n=0;n<full.size();++n)full[n]=event(n/16,0x90|(n/128),n%128,100);
    check(tools.process(128,full,output)&&tools.stats().held==midi::Tools::voices,"Voice ledger did not admit its declared capacity");
    output.clear();const std::array extra{event(0,0x90,60,100)};check(!tools.process(64,extra,output),"Full ownership ledger silently dropped a note-on");output.clear();check(tools.process(64,{},output)&&tools.stats().held==0,"Ledger exhaustion did not recover");
    unsigned seed=0x64ce11u;auto random=[&]{seed^=seed<<13;seed^=seed>>17;seed^=seed<<5;return seed;};std::array<MidiEvent,64> batch{};
    for(unsigned block=0;block<512;++block){midi::ToolsConfig config;config.transpose=int(random()%49)-24;config.outputChannel=random()%17;config.inputChannel=random()%17;config.velocityLow=1+random()%64;config.scale=block%2?0xab5:0xfff;config.chord={0,4,7,12};config.chordVoices=1+random()%4;check(tools.configure(config),"Random valid MIDI config rejected");
        for(unsigned n=0;n<batch.size();++n){const auto kind=0x80+(random()%7)*16;batch[n]=event(n,kind|(random()%16),random()%128,random()%128,kind==0xc0||kind==0xd0?2:3);}
        output.clear();const bool ok=tools.process(64,batch,output);if(!ok)output.emergency();std::uint32_t previous{};for(const auto& e:output.events()){check(midi::valid(e)&&e.frame>=previous&&e.frame<64,"Random MIDI emitted invalid/unordered events");previous=e.frame;}const auto stats=tools.stats();check(stats.held<=midi::Tools::voices&&stats.sounding<=2048,"Random MIDI corrupted ownership counts");
    }
    tools.requestPanic();output.clear();check(tools.process(64,{},output)&&tools.stats().held==0&&tools.stats().sounding==0,"Random MIDI left notes after panic");
}
class Transform final:public Processor {
public:
    midi::Tools tools;
    void prepare(const ProcessSpec&)override{}
    void reset()noexcept override{tools.reset();}
    bool routesMidi()const noexcept override{return true;}
    void process(AudioBlock<float>,const ParameterBank&,const Transport&,std::span<const MidiEvent>)noexcept override{}
    void processMidi(AudioBlock<float> audio,const ParameterBank& bank,const Transport& transport,std::span<const MidiEvent> input,AudioBlock<float>,midi::Output& output)noexcept override{midi::ToolsConfig config;config.transpose=int(bank.get(1));tools.configure(config);if(!tools.process(audio.frames,input,output,transport))output.reject();}
};
void automation(){
    const std::array<Parameter,1> parameters{{{1,"transpose","Transpose","",-48,48,0,1}}};PluginDescriptor descriptor{"test.midi","MIDI test","Test","1",2,parameters,true,100,100,false,true};Engine engine(descriptor,std::make_unique<Transform>());engine.prepare(48000,64,2);AudioBuffer<float> audio;audio.resize(2,64);for(unsigned ch=0;ch<2;++ch)std::fill_n(audio.block().channel(ch),64,.125f);midi::Buffer output;output.prepare();
    const std::array input{event(3,0x90,60,100),event(17,0x90,60,90),event(44,0x80,60),event(61,0x80,60)};const std::array<ParameterEvent,1> changes{{{16,1,12}}};
    cell_test::audio=true;cell_test::allocations=cell_test::deletions=0;engine.process(audio.block(),changes,{},input,{},&output);cell_test::audio=false;
    const auto out=output.events();check(out.size()==4&&out[0].frame==3&&out[0].data[1]==60&&out[1].frame==17&&out[1].data[1]==72&&out[2].frame==44&&out[2].data[1]==60&&out[3].frame==61&&out[3].data[1]==72,"Sample-accurate automation/rebasing broke held-note mapping");check(!cell_test::allocations&&!cell_test::deletions,"Routed Engine callback allocated");for(unsigned ch=0;ch<2;++ch)for(unsigned n=0;n<64;++n)check(audio.block().channel(ch)[n]==.125f,"MIDI processing changed audio");
}
}
int main(){try{transparency();mappingOwnership();collisionsAndFilters();harmonyVelocityAndPressure();controllersAndPedal();safety();overflow();poolAndRandomizedEvents();automation();std::cout<<"PASS MIDI ownership, routing, harmony, controls, panic, overflow, randomized events and sample-accurate automation\n";}catch(const std::exception& e){cell_test::audio=false;std::cerr<<e.what()<<'\n';return 1;}}
