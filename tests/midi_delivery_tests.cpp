#include <cell/midi_delivery.hpp>
#include "allocation_watch.hpp"
#include <iostream>
#include <stdexcept>
using namespace cell;
void check(bool b,const char* m){if(!b)throw std::runtime_error(m);}
struct Sink:midi::Output{std::array<MidiEvent,100> events{};unsigned count{},limit=100;bool push(MidiEvent e)noexcept override{if(count>=limit)return false;events[count++]=e;return true;}};
void checkSysEx() {
    const std::array<std::uint8_t, 6> expected{0xf0, 0x7d, 0, 42, 127, 0xf7};
    auto bytes = expected;
    const auto event = MidiEvent::systemExclusive(19, bytes);
    constexpr std::array<std::uint8_t, 3> invalid{0xf0, 0x81, 0xf7};
    const auto malformed = MidiEvent::systemExclusive(17, invalid);
    midi::Buffer buffer;
    buffer.prepare(48, expected.size());
    midi::Delivery delivery;
    cell_test::audio = true;
    delivery.begin(&buffer, 64);
    check(!delivery.push(malformed) && !delivery.pending(), "Malformed SysEx started a note panic");
    check(!delivery.push(MidiEvent::systemExclusive(18, {})) && !delivery.pending(), "Empty SysEx started a note panic");
    check(delivery.push(event), "SysEx did not reach a prepared sink");
    check(!delivery.push(event) && !delivery.pending(), "SysEx capacity failure started a note panic");
    check(delivery.push({20, {0x92, 60, 100}, 3}), "Rejected SysEx blocked channel MIDI");
    cell_test::audio = false;
    bytes.fill(0);
    check(buffer.events().size() == 2 && buffer.events()[0].frame == 19 &&
          std::ranges::equal(expected, buffer.events()[0].sysEx), "SysEx bytes outlived their producer incorrectly");
    auto snapshot = buffer;
    buffer.clear();
    check(buffer.push(MidiEvent::systemExclusive(19, expected)), "Could not reuse the SysEx arena");
    check(snapshot.events()[0].sysEx.data() != buffer.events()[0].sysEx.data(), "Copying a MIDI buffer aliased the SysEx arena");
    check(!cell_test::allocations && !cell_test::deletions, "SysEx allocated or freed memory in the callback");
}
int main(){try{midi::Delivery out;Sink sink;cell_test::audio=true;out.begin(&sink,64);check(out.push({17,{0x92,60,100},3}),"Note output failed");sink.limit=1;check(!out.push({18,{0x82,60,0},3})&&out.pending(),"Rejected note-off was lost");sink.count=0;sink.limit=5;out.begin(&sink,64);check(sink.count==5&&!out.push({20,{0x92,65,100},3}),"Recovery admitted notes before panic finished");sink.count=0;sink.limit=100;out.begin(&sink,64);check(sink.count==43&&!out.pending(),"Recovery did not resume after backpressure");check(out.push({63,{0xe2,0,64},3}),"Pitch wheel failed");out.begin(nullptr,64);check(!out.push({0,{0x82,60,0},3}),"Missing output silently accepted note-off");out.begin(&sink,64);check(!out.pending(),"Reconnect did not clear hanging notes");check(out.push({0,{0x90,64,90},3}),"Reconnected note failed");out.reset();sink.count=0;out.begin(&sink,64);check(sink.count==48,"Lifecycle reset did not recover");check(!out.push({64,{0x90,60,100},3}),"Out of block event accepted");cell_test::audio=false;check(!cell_test::allocations&&!cell_test::deletions,"MIDI delivery allocated on audio");checkSysEx();std::cout<<"PASS bounded SysEx and MIDI delivery, absent sink, rejected note-offs, incremental panic recovery, reset and allocation guard\n";}catch(const std::exception& e){cell_test::audio=false;std::cerr<<e.what()<<'\n';return 1;}}
