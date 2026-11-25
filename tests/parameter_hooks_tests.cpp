// SPDX-License-Identifier: Apache-2.0
#include <cell/plugin.hpp>
#include <cell/parameter_restore.hpp>
#include "allocation_watch.hpp"
#include <array>
#include <iostream>
#include <limits>
namespace {
void check(bool value,const char* message){if(!value)throw std::runtime_error(message);}
constexpr auto gain=cell::stableId("hook.gain");
const std::array<cell::Parameter,1> fields{{{gain,"hook.gain","Gain","",0,2,1,0,1,true,false}}};
class Processor final:public cell::Processor {
public:
 void attachParameters(const cell::ParameterBank& bank)override{attached=&bank;}
 void initializeParameters(cell::ParameterBank& bank)override{check(attached==&bank,"Seed hook ran before parameter attachment");++seeds;check(bank.restorePlain(gain,.25),"Seed hook could not set a physical value");}
 void prepare(const cell::ProcessSpec&)override{}
 void reset()noexcept override{}
 void parameterChanged(cell::ParamId id,double value)noexcept override{if(count<changes.size())changes[count++]={id,value,rendered};}
 void process(cell::AudioBlock<float> block,const cell::ParameterBank& bank,const cell::Transport&,std::span<const cell::MidiEvent>)noexcept override{block.multiply(float(bank.get(gain)));rendered+=block.frames;}
 struct Change{cell::ParamId id;double value;unsigned frame;};std::array<Change,4> changes{};unsigned count{},seeds{},rendered{};const cell::ParameterBank* attached{};
};
}
int main(){try{
 const cell::Parameter frequency{1,"frequency","Frequency","Hz",20,20000,1000,1,.3,true,false};
 check(cell::state::compatibleParameterValue(frequency,999.99993896484375)==1000,"Legacy float residue rejected a valid whole-Hz setting");
 check(cell::state::compatibleParameterValue(frequency,3840.000244140625)==3840,"Legacy crossover float residue was not repaired");
 check(cell::state::compatibleParameterValue(frequency,19999.998046875)==20000,"Legacy high-frequency float residue was not repaired");
 check(!cell::state::compatibleParameterValue(frequency,999.95)&&!cell::state::compatibleParameterValue(frequency,20001),"Legacy tolerance admitted a materially different frequency");
 const cell::Parameter continuous{2,"gain","Gain","dB",-24,24,0,.01,1,true,false};
 check(cell::state::compatibleParameterValue(continuous,.123456789)==.123456789,"Legacy import rounded a continuous saved setting");
 check(cell::state::compatibleParameterValue(continuous,24.000001)==24&&!cell::state::compatibleParameterValue(continuous,24.01),"Legacy endpoint tolerance is too strict or too broad");
 const cell::Parameter mode{3,"mode","Mode","",0,4,0,1,1,true,false};
 check(!cell::state::compatibleParameterValue(mode,1.1)&&!cell::state::compatibleParameterValue(mode,std::numeric_limits<double>::infinity()),"Legacy tolerance accepted a fractional mode or infinity");
 auto choices=mode;choices.unit="mode";choices.choices={"Mono","Stereo","Symphonic","Tape","Chorus"};
 for(unsigned i=0;i<choices.choices.size();++i)
     check(choices.parseText(choices.text(i))==i,"Choice did not round-trip through host text");
 check(choices.parseText(" \tSyMPHONIC mode\n")==2&&continuous.parseText(" +6.0 db ")==6&&
       !continuous.parseText("6 Hz"),"Host parameter text rejected a choice or accepted the wrong unit");
 choices.choices[3]="SYMPHONIC";
 check(!choices.parseText("Symphonic"),"Ambiguous choice label was accepted");
 cell::PluginDescriptor descriptor{"cell.test.hooks","Hooks","Cell","1",2,fields};auto owner=std::make_unique<Processor>();auto* processor=owner.get();cell::Engine engine(descriptor,std::move(owner));check(processor->seeds==1&&engine.parameters().get(gain)==.25,"Construction did not seed exactly once");engine.prepare(48000,8,2);cell::AudioBuffer<float> audio(2,8);for(unsigned c=0;c<2;++c)std::fill_n(audio.block().channel(c),8,1);
 const std::array<cell::ParameterEvent,5> events{{{0,123,1},{0,gain,std::numeric_limits<double>::quiet_NaN()},{4,gain,1.5},{8,gain,9},{9,gain,0}}};
 cell_test::allocations=cell_test::deletions=0;cell_test::audio=true;engine.process(audio.block(),events);cell_test::audio=false;check(!cell_test::allocations&&!cell_test::deletions,"Parameter hook path allocated or reclaimed on audio");check(processor->count==2&&processor->changes[0].id==gain&&processor->changes[0].value==1.5&&processor->changes[0].frame==4&&processor->changes[1].value==2&&processor->changes[1].frame==8,"Hook received an invalid, unclamped or mistimed host event");for(unsigned i=0;i<8;++i)check(audio.block().channel(0)[i]==(i<4?.25f:1.5f),"Host hook changed sample-accurate event ordering");
 check(engine.editGesture(gain,.75),"GUI gesture rejected");check(processor->count==2,"GUI edit was misreported as host playback");engine.prepare(48000,8,2);check(processor->seeds==1&&engine.parameters().get(gain)==.75,"Repreparation reapplied construction defaults");std::cout<<"PASS construction seeding, accepted/clamped sample-accurate host hooks, GUI separation and allocation-free callbacks\n";
 }catch(const std::exception& error){cell_test::audio=false;std::cerr<<error.what()<<'\n';return 1;}}
