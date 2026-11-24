#include <cell/latest_prepared_exchange.hpp>
#include "allocation_watch.hpp"
#include <iostream>
#include <stdexcept>
#include <thread>

namespace {
void check(bool ok,const char* message){if(!ok)throw std::runtime_error(message);}
struct Resource {
    explicit Resource(unsigned n):value(n),guard(n^0x719abcd){++alive;auto peak=maximum.load();while(peak<alive&&!maximum.compare_exchange_weak(peak,alive.load())){}}
    ~Resource(){--alive;if(cell_test::audio)++wrongThread;}
    unsigned value{},guard{};
    static inline std::atomic<unsigned> alive{},maximum{},wrongThread{};
};
template<std::size_t Slots> void suspended(){
    cell::LatestPreparedExchange<Resource,Slots> exchange;
    auto initial=std::make_unique<Resource>(0);check(exchange.publishLatest(initial,[](Resource&,auto)noexcept{}),"initial publication rejected");
    auto* active=exchange.audioUpdate();check(active&&active->value==0,"initial rack not adopted");
    for(unsigned n=1;n<=10000;++n){auto next=std::make_unique<Resource>(n);check(exchange.publishLatest(next,[](Resource&,auto)noexcept{})&&!next,"suspended publication exhausted its slots");check(active->value==0&&active->guard==0x719abcd,"producer changed the active audio borrow");}
    cell_test::allocations=cell_test::deletions=0;cell_test::audio=true;auto* newest=exchange.audioUpdate();const bool correct=newest&&newest->value==10000;cell_test::audio=false;
    check(correct,"resume did not adopt the newest accepted rack");check(!cell_test::allocations&&!cell_test::deletions,"audio adoption allocated or reclaimed a rack");check(exchange.reclaim()==1,"previous audio rack was not retired");
    const auto before=Resource::alive.load();std::unique_ptr<Resource> empty;check(!exchange.publishLatest(empty,[](Resource&,auto)noexcept{}),"null publication accepted");check(Resource::alive==before,"failed publication changed ownership");
}
void retained(){
    cell::LatestPreparedExchange<Resource> exchange;
    auto publish=[&](unsigned n){auto value=std::make_unique<Resource>(n);check(exchange.publishLatest(value,[](Resource&,auto)noexcept{})&&!value,"Retained transition rejected a coalesced publication");};
    publish(1);auto* original=exchange.audioUpdateRetainingPrevious();check(original&&original->value==1&&!exchange.audioPrevious(),"First adoption fabricated a previous borrow");
    publish(2);cell_test::allocations=cell_test::deletions=0;cell_test::audio=true;auto* current=exchange.audioUpdateRetainingPrevious();auto* previous=exchange.audioPrevious();cell_test::audio=false;
    check(current&&current->value==2&&previous==original,"Crossfade lost its previous resource");check(!cell_test::allocations&&!cell_test::deletions,"Retained adoption allocated or reclaimed audio-owned memory");
    for(unsigned n=3;n<=10000;++n){publish(n);check(previous->value==1&&current->value==2,"Producer reclaimed a retained borrow");}
    cell_test::allocations=cell_test::deletions=0;cell_test::audio=true;check(exchange.audioUpdate()==current&&exchange.audioUpdateRetainingPrevious()==current,"Pending update interrupted an unfinished crossfade");const bool released=exchange.audioReleasePrevious();cell_test::audio=false;
    check(released&&!exchange.audioPrevious()&&!cell_test::allocations&&!cell_test::deletions,"Previous release destroyed a resource on audio");check(exchange.reclaim()==1,"Producer could not reclaim the finished crossfade");
    cell_test::audio=true;auto* newest=exchange.audioUpdateRetainingPrevious();previous=exchange.audioPrevious();cell_test::audio=false;check(newest&&newest->value==10000&&previous==current,"Coalesced update did not follow the crossfade");
    cell_test::audio=true;const bool finalRelease=exchange.audioReleasePrevious();cell_test::audio=false;check(finalRelease,"Final previous borrow could not be released");exchange.reclaim();
}
void concurrent(){
    cell::LatestPreparedExchange<Resource> exchange;std::atomic<bool> done{},bad{};unsigned rejected{};
    std::thread consumer([&]{unsigned previous=0;cell_test::audio=true;cell_test::allocations=cell_test::deletions=0;for(;;){auto* p=exchange.audioUpdate();if(p){if(p->guard!=(p->value^0x719abcd)||p->value<previous)bad=true;previous=p->value;}if(done.load(std::memory_order_acquire)&&previous==50000)break;std::this_thread::yield();}if(cell_test::allocations||cell_test::deletions)bad=true;cell_test::audio=false;});
    for(unsigned n=1;n<=50000;++n){auto next=std::make_unique<Resource>(n);while(!exchange.publishLatest(next,[](Resource&,auto)noexcept{})){++rejected;std::this_thread::yield();}}done.store(true,std::memory_order_release);consumer.join();exchange.reclaim();check(!bad,"concurrent rack ownership/order was corrupted");check(!rejected,"Three-slot latest handoff rejected a valid concurrent publication");
}
}
int main(){try{suspended<2>();suspended<3>();retained();concurrent();check(Resource::alive==0&&!Resource::wrongThread,"resource leak or audio-thread destruction");check(Resource::maximum<=4,"pending storage grew beyond three slots plus the incoming candidate");std::cout<<"PASS: suspended latest-rack coalescing, bounded storage, resume, 50000 concurrent publications and audio allocation/destruction guards\n";}catch(const std::exception& e){cell_test::audio=false;std::cerr<<e.what()<<'\n';return 1;}}
