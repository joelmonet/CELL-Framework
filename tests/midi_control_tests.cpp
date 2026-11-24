#include <cell/plugin.hpp>
#include "allocation_watch.hpp"
#include <iostream>

using namespace cell;
namespace {
void check(bool value,const char* why){if(!value)throw std::runtime_error(why);}
midi::ControlMessage message(std::uint64_t owner,std::uint64_t sequence,midi::ControlAction action,unsigned status=0,unsigned note=0,unsigned velocity=0){return {owner,sequence,action,{0,{std::uint8_t(status),std::uint8_t(note),std::uint8_t(velocity)},3}};}
void ownership(){
    midi::ControlInput input;using A=midi::ControlAction;
    check(input.submit(message(1,1,A::open))&&input.submit(message(1,2,A::event,0x90,60,100)),"Controller note admission failed");
    auto events=input.drain();check(events.size()==1&&events[0].data[0]==0x90,"Controller note did not reach audio");
    check(input.submit(message(2,1,A::open)),"New controller could not take ownership");
    check(!input.submit(message(1,99,A::event,0x90,61,100))&&!input.submit(message(1,100,A::close)),"Old controller retained input ownership");
    check(input.submit(message(2,2,A::event,0x91,64,100)),"New controller note failed");events=input.drain();check(events.size()==2&&events[0].data[0]==0x80&&events[0].data[1]==60&&events[1].data[0]==0x91,"Controller takeover did not release its predecessor before playing");
    check(!input.submit(message(2,2,A::event,0x91,65,100)),"Repeated controller sequence retriggered a note");
    check(input.submit(message(2,3,A::close))&&!input.submit(message(2,4,A::event,0x91,65,100)),"Closed controller accepted delayed notes");events=input.drain();check(events.size()==1&&events[0].data[0]==0x81&&events[0].data[1]==64,"Controller close lost a note-off");
    midi::ControlInput fresh;check(fresh.submit(message(10,8,A::event,0x90,60,100)),"A current view could not reacquire a fresh prepared instrument");check(fresh.drain().size()==1,"Reacquired controller did not play");
    check(!fresh.submit(message(11,1,A::event,0x90,200,100))&&fresh.submit(message(10,9,A::event,0x80,60,0)),"Invalid newer input stole ownership");fresh.drain();
    midi::ControlInput reordered;check(reordered.submit(message(12,9,A::close))&&!reordered.submit(message(12,1,A::open))&&!reordered.submit(message(12,10,A::event,0x90,60,100)),"A delayed open/note bypassed a close tombstone");
}
void saturation(){
    midi::ControlInput input;using A=midi::ControlAction;check(input.submit(message(1,1,A::open))&&input.submit(message(1,2,A::event,0x90,60,100)),"Saturation fixture failed");input.drain();
    for(unsigned n=0;n<input.queueCapacity;++n)check(input.submit(message(1,n+3,A::event,0x90,61,100)),"Controller queue filled too early");
    check(!input.submit(message(1,999,A::event,0x90,62,100)),"Controller queue admitted an overflowing note");
    const auto events=input.drain();check(events.size()==1&&events[0].data[0]==0x80&&events[0].data[1]==60,"Full queue replayed stale notes or lost its capacity-independent release");
}
class Capture final:public Processor {
public:
    std::array<MidiEvent,256> seen{};unsigned count{},resets{};
    void prepare(const ProcessSpec&)override{}
    void reset()noexcept override{++resets;}
    void process(AudioBlock<float>,const ParameterBank&,const Transport& transport,std::span<const MidiEvent> events)noexcept override{for(auto event:events)if(count<seen.size()){event.frame+=unsigned(transport.samplePosition);seen[count++]=event;}}
};
void engine(){
    const std::array<Parameter,1> fields{{{1,"gain","Gain","",0,1,0,0,1,true,false}}};const PluginDescriptor descriptor{"test.controller-midi","Controller input","Cell","1",2,fields,false,1000,620,false,true};
    auto capture=std::make_unique<Capture>();auto* observer=capture.get();Engine engine(descriptor,std::move(capture));engine.prepare(48000,64);using A=midi::ControlAction;
    check(engine.controllerMidi(message(3,1,A::open))&&engine.controllerMidi(message(3,2,A::event,0x90,60,100)),"Engine rejected instrument controller input");
    const std::array<MidiEvent,2> host{{{10,{0x90,70,100},3},{30,{0x80,70,0},3}}};const ParameterEvent edit{17,1,.5};AudioBuffer<float> audio;audio.resize(2,64);
    cell_test::allocations=cell_test::deletions=0;cell_test::audio=true;engine.process(audio.block(),std::span(&edit,1),{},host);cell_test::audio=false;
    check(!cell_test::allocations&&!cell_test::deletions,"Controller merge allocated or reclaimed on audio");
    check(observer->count==3&&observer->seen[0].frame==0&&observer->seen[0].data[1]==60&&observer->seen[1].frame==10&&observer->seen[2].frame==30,"Controller merge changed host timestamps across parameter splits");
    check(engine.controllerMidi(message(3,3,A::releaseAll)),"Engine release request failed");engine.process(audio.block());check(observer->count==4&&observer->seen[3].data[0]==0x80&&observer->seen[3].data[1]==60,"Engine release failed to reach the instrument");
    check(engine.controllerMidi(message(3,4,A::event,0x90,62,100)),"Pending controller fixture failed");const auto saved=engine.saveState();check(engine.loadState(saved),"Controller state fixture did not reopen");engine.process(audio.block());check(observer->count==4,"Preset restore replayed pending GUI notes");
}
}
int main(){try{ownership();saturation();engine();std::cout<<"PASS controller ownership, release on takeover/close, queue saturation, host MIDI merge, sample offsets, preset cleanup and callback allocation guard\n";}catch(const std::exception& e){cell_test::audio=false;std::cerr<<e.what()<<'\n';return 1;}}
