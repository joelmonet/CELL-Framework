// SPDX-License-Identifier: Apache-2.0
#include <cell/analysis_lease.hpp>
#include "allocation_watch.hpp"
#include <iostream>
#include <limits>
#include <stdexcept>
#include <thread>
namespace {
void check(bool ok,const char* message){if(!ok)throw std::runtime_error(message);}
void boundaries(){
 cell::AnalysisLease lease;check(!lease.active()&&!lease.consume(64),"Unrequested analysis was enabled");lease.renew(100);check(lease.active()&&!lease.consume(0),"Empty block spent or enabled analysis");check(lease.consume(64)&&lease.consume(64)&&!lease.consume(64)&&!lease.active(),"Partial final block did not expire");
 lease.renew(32);lease.stop();check(!lease.consume(64),"Stopped lease admitted a later block");lease.renew(0);check(!lease.consume(64),"Zero renewal enabled analysis");lease.renew(std::numeric_limits<std::uint32_t>::max());check(lease.consume(std::numeric_limits<std::uint32_t>::max()-1)&&lease.consume(64)&&!lease.consume(64),"Large lease overflowed or renewed itself");
}
void callbackStorage(){cell::AnalysisLease lease;lease.renew(48000);cell_test::allocations=cell_test::deletions=0;cell_test::audio=true;unsigned admitted=0;for(unsigned n=0;n<1000;++n)admitted+=lease.consume(257);cell_test::audio=false;check(admitted==187&&!cell_test::allocations&&!cell_test::deletions,"Analysis lease allocated, reclaimed or admitted extra blocks");}
void concurrent(){
 cell::AnalysisLease lease;std::atomic<bool> start=false,done=false,bad=false;
 std::thread audio([&]{while(!start.load(std::memory_order_acquire))std::this_thread::yield();cell_test::audio=true;cell_test::allocations=cell_test::deletions=0;for(unsigned n=0;n<50000||!done.load(std::memory_order_acquire);++n)lease.consume(257);if(cell_test::allocations||cell_test::deletions)bad=true;cell_test::audio=false;});
 start.store(true,std::memory_order_release);for(unsigned n=0;n<30000;++n){lease.renew(48000);if(n%3==0)lease.stop();}lease.stop();done.store(true,std::memory_order_release);audio.join();check(!bad&&!lease.active()&&!lease.consume(257),"Concurrent renewal/stop left analysis active or allocated");
}
}
int main(){try{boundaries();callbackStorage();concurrent();std::cout<<"PASS bounded analysis lease, stop/renew races and callback storage\n";}catch(const std::exception& e){cell_test::audio=false;std::cerr<<e.what()<<'\n';return 1;}}
