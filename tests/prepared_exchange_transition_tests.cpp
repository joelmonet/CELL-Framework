// Independent Cell framework fixture; core licensing follows the root LICENSE.
// Source fixture only: uncompiled/unrun while suite compilation is held.
#include <cell/prepared_exchange.hpp>
#include "allocation_watch.hpp"
#include <iostream>
#include <thread>
using namespace cell;
namespace {
void check(bool value,const char* message){if(!value)throw std::runtime_error(message);}
struct Value {explicit Value(unsigned n):number(n){}~Value(){++destroyed;if(cell_test::audio)++audioDestructions;}unsigned number;static inline std::atomic<unsigned> destroyed{},audioDestructions{};};
void ownership(){
    Value::destroyed=Value::audioDestructions=0;
    PreparedExchange<Value> exchange;auto a=std::make_unique<Value>(1);check(exchange.publish(exchange.request(),a),"First handoff not published");
    cell_test::audio=true;auto* first=exchange.audioUpdateRetainingPrevious();cell_test::audio=false;check(first&&first->number==1&&!exchange.audioPrevious(),"First handoff has an invalid previous borrow");
    auto b=std::make_unique<Value>(2);check(exchange.publish(exchange.request(),b),"Second handoff not published");cell_test::audio=true;auto* current=exchange.audioUpdateRetainingPrevious();cell_test::audio=false;
    check(current&&current->number==2&&exchange.audioPrevious()==first,"Old DSP was not retained for the fade");check(exchange.reclaim()==0&&first->number==1&&Value::destroyed==0,"Producer reclaimed a borrowed transition object");
    auto c=std::make_unique<Value>(3);check(exchange.publish(exchange.request(),c),"Queued handoff not published");check(exchange.audioUpdateRetainingPrevious()==current&&exchange.audioUpdate()==current,"Queued update replaced a running fade");
    cell_test::allocations=cell_test::deletions=0;cell_test::audio=true;check(exchange.audioReleasePrevious(),"Old DSP could not be retired");cell_test::audio=false;
    check(!cell_test::allocations&&!cell_test::deletions&&!Value::audioDestructions,"Audio transition release allocated or destroyed DSP");check(exchange.reclaim()==1&&Value::destroyed==1,"Producer did not reclaim released DSP");
    check(exchange.audioUpdateRetainingPrevious()->number==3&&exchange.audioPrevious()==current,"Queued current generation was not adopted");check(exchange.audioReleasePrevious(),"Second transition release failed");exchange.reclaim();
}
void cancellationAndPressure(){
    PreparedExchange<Value,2> exchange;auto a=std::make_unique<Value>(11);check(exchange.publish(exchange.request(),a),"Initial candidate rejected");exchange.audioUpdateRetainingPrevious();
    auto b=std::make_unique<Value>(22);check(exchange.publish(exchange.request(),b),"Transition candidate rejected");exchange.audioUpdateRetainingPrevious();auto c=std::make_unique<Value>(33);const auto ticket=exchange.request();
    check(!exchange.publish(ticket,c)&&c&&exchange.audioCurrent()->number==22&&exchange.audioPrevious()->number==11,"Backpressure stole ownership or lost running DSP");check(exchange.audioReleasePrevious(),"Pressure release failed");exchange.reclaim();check(exchange.publish(ticket,c),"Released capacity did not accept the current ticket");
    exchange.request();check(exchange.audioUpdateRetainingPrevious()->number==22&&!exchange.audioPrevious(),"Cancelled queued candidate became audible");exchange.reclaim();
}
void concurrent(){
    Value::audioDestructions=0;PreparedExchange<Value> exchange;std::atomic<bool> complete{};std::atomic<unsigned> maximum{};
    std::thread producer([&]{for(unsigned i=1;i<=10000;++i){auto value=std::make_unique<Value>(i);const auto ticket=exchange.request();while(!exchange.publish(ticket,value))std::this_thread::yield();}complete.store(true,std::memory_order_release);});
    unsigned frames=0;bool ordered=true;cell_test::allocations=cell_test::deletions=0;cell_test::audio=true;
    while(!complete.load(std::memory_order_acquire)||maximum.load()<10000){
        if(auto* value=exchange.audioUpdateRetainingPrevious()){maximum.store(std::max(maximum.load(),value->number));if(auto* old=exchange.audioPrevious())ordered&=old->number<=value->number;}
        if(++frames%17==0)exchange.audioReleasePrevious();
    }
    exchange.audioReleasePrevious();cell_test::audio=false;producer.join();exchange.reclaim();check(ordered,"Transition ownership crossed publication order");check(!Value::audioDestructions&&!cell_test::allocations&&!cell_test::deletions,"Concurrent audio handoff allocated/reclaimed");
}
}
int main(){try{ownership();cancellationAndPressure();concurrent();std::cout<<"PASS: retained DSP handoff, cancellation/backpressure and producer-only reclamation\n";}catch(const std::exception& error){cell_test::audio=false;std::cerr<<error.what()<<'\n';return 1;}}
