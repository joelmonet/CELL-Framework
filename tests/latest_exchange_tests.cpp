#include <cell/latest_exchange.hpp>
#include "allocation_watch.hpp"
#include <array>
#include <iostream>
#include <thread>
#include <chrono>
namespace {
void check(bool value,const char* message){if(!value)throw std::runtime_error(message);}
struct Object {
    inline static std::atomic<unsigned> alive{},destroyedOnAudio{};
    const unsigned revision;
    std::array<unsigned,64> guard;
    explicit Object(unsigned n):revision(n){++alive;guard.fill(n^0x3ac5174bu);}
    ~Object(){--alive;if(cell_test::audio)++destroyedOnAudio;}
    bool valid()const{for(auto word:guard)if(word!=(revision^0x3ac5174bu))return false;return true;}
};
}
int main(){try{
    {
        cell::LatestExchange<Object> exchange;
        for(unsigned n=1;n<=1000;++n){auto next=std::make_unique<Object>(n);check(exchange.publish(next)&&!next,"Publication did not transfer ownership");check(Object::alive==1,"Superseded unpublished candidates accumulated");}
        cell_test::allocations=cell_test::deletions=0;cell_test::audio=true;auto* selected=exchange.audioUpdate();const bool correct=selected&&selected->revision==1000&&selected->valid();cell_test::audio=false;
        check(correct&&!cell_test::allocations&&!cell_test::deletions,"Burst publication lost the latest candidate or used callback ownership");
        auto rejected=std::make_unique<Object>(1001);cell_test::failAfter=0;bool failed=false;try{exchange.publish(rejected);}catch(const std::bad_alloc&){failed=true;}cell_test::failAfter=-1;
        check(failed&&rejected&&exchange.audioCurrent()==selected&&selected->valid(),"Failed publication changed the accepted object");
    }
    check(!Object::alive,"Stopped exchange retained objects");
    {
        cell::LatestExchange<Object> exchange;std::atomic<bool> done{},invalid{},callbackHeap{};std::atomic<unsigned> last{};
        std::thread consumer([&]{const auto deadline=std::chrono::steady_clock::now()+std::chrono::seconds(10);while(!done.load(std::memory_order_acquire)||last<50000){if(std::chrono::steady_clock::now()>deadline){invalid=true;break;}cell_test::allocations=cell_test::deletions=0;cell_test::audio=true;auto* selected=exchange.audioUpdate();if(selected){if(!selected->valid()||selected->revision<last.load())invalid=true;last=selected->revision;}if(cell_test::allocations||cell_test::deletions)callbackHeap=true;cell_test::audio=false;std::this_thread::yield();}});
        for(unsigned n=1;n<=50000;++n){auto next=std::make_unique<Object>(n);exchange.publish(next);}
        done.store(true,std::memory_order_release);consumer.join();exchange.reclaim();
        check(last==50000&&!invalid&&!callbackHeap&&!Object::destroyedOnAudio,"Concurrent latest exchange violated ownership, publication or order");
    }
    check(!Object::alive,"Concurrent exchange leaked prepared objects");std::cout<<"PASS arbitrary pre-callback bursts, atomic allocation failure, 50000 concurrent publications and producer-only reclamation\n";
}catch(const std::exception& error){cell_test::audio=false;cell_test::failAfter=-1;std::cerr<<error.what()<<'\n';return 1;}}
