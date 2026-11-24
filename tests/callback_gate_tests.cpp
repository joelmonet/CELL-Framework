#include <cell/callback_gate.hpp>
#include "allocation_watch.hpp"
#include <array>
#include <iostream>
#include <thread>

namespace {
void check(bool value,const char* text){if(!value)throw std::runtime_error(text);}
void basic(){
    cell::CallbackGate gate;
    {cell::CallbackGate::PauseScope pause(gate);check(bool(pause),"Idle callback gate did not pause");cell::CallbackGate::AudioScope blocked(gate);check(!blocked,"Callback entered paused state");cell::CallbackGate::PauseScope nested(gate);check(!nested,"A second controller stole the pause");}
    cell_test::audio=true;cell_test::allocations=cell_test::deletions=0;
    {cell::CallbackGate::AudioScope callback(gate);if(!callback){cell_test::audio=false;throw std::runtime_error("Callback did not resume");}}
    cell_test::audio=false;check(!cell_test::allocations&&!cell_test::deletions,"Callback gate used heap ownership");
    {cell::CallbackGate::PauseScope optional(gate,false);cell::CallbackGate::AudioScope callback(gate);check(bool(optional)&&bool(callback),"Unneeded pause interrupted a publication-capable product");}
}
void timeout(){
    cell::CallbackGate gate;std::atomic<bool> entered{},leave{};
    std::thread audio([&]{cell::CallbackGate::AudioScope callback(gate);entered.store(bool(callback),std::memory_order_release);while(!leave.load(std::memory_order_acquire))std::this_thread::yield();});
    while(!entered.load(std::memory_order_acquire))std::this_thread::yield();
    bool rejected=false;{cell::CallbackGate::PauseScope pause(gate,true,std::chrono::milliseconds(0));rejected=!pause;}
    leave.store(true,std::memory_order_release);audio.join();check(rejected,"Busy callback was treated as quiescent");
    cell::CallbackGate::AudioScope resumed(gate);check(bool(resumed),"Timed-out pause left callback entry disabled");
}
void concurrency(){
    cell::CallbackGate gate;std::array<std::uint64_t,2> payload{0,~std::uint64_t(0)};std::atomic<bool> stop{},failed{};std::atomic<unsigned> calls{};
    std::thread audio([&]{cell_test::audio=true;while(!stop.load(std::memory_order_acquire)){cell::CallbackGate::AudioScope callback(gate);if(callback){if(payload[0]!=~payload[1])failed=true;++calls;}}if(cell_test::allocations||cell_test::deletions)failed=true;cell_test::audio=false;});
    while(!calls.load(std::memory_order_acquire))std::this_thread::yield();
    for(std::uint64_t value=1;value<=20000;++value){cell::CallbackGate::PauseScope pause(gate);if(!pause){failed=true;break;}payload={value,~value};}
    stop.store(true,std::memory_order_release);audio.join();check(!failed&&calls>0,"Control replacement raced callback data or used callback heap ownership");
}
}
int main(){try{basic();timeout();concurrency();std::cout<<"PASS callback quiescence, nonblocking audio entry, control contention/timeout, 20000 concurrent state replacements and zero callback heap ownership\n";}catch(const std::exception& e){cell_test::audio=false;std::cerr<<e.what()<<'\n';return 1;}}
