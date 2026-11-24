#include <cell/parameter_timeline.hpp>
#include "allocation_watch.hpp"
#include <iostream>
#include <thread>

namespace {
using cell::wrapper::ParameterTimeline;
void check(bool condition, const char* message) { if (!condition) throw std::runtime_error(message); }
const std::array<cell::Parameter, 2> descriptors{{{17,"first","First","",0,1,0}, {18,"second","Second","",0,1,0}}};
void timingAndAdmission() {
    cell::ParameterBank bank(descriptors); ParameterTimeline timeline;
    const std::array<ParameterTimeline::Value, 2> a{{{17,.25},{18,.75}}}, b{{{17,.5},{18,1}}};
    const auto first = timeline.publish(bank,a), second = timeline.publish(bank,b);
    check(first && second && *first < *second, "timeline sequence");
    cell::ParameterBank other(descriptors);
    check(!timeline.publish(other,a)&&!timeline.applyThrough(*first,other)&&other.get(17)==0,"timeline crossed processor parameter banks");
    check(timeline.applyThrough(0,bank) && bank.get(17)==0, "future update applied early");
    cell_test::allocations=cell_test::deletions=0; cell_test::audio=true;
    const auto applied=timeline.applyThrough(*first,bank);
    cell_test::audio=false;
    check(applied && cell_test::allocations==0 && cell_test::deletions==0, "audio adoption allocated or retired synchronously");
    check(bank.get(17)==.25 && bank.get(18)==.75, "wrong timed state");
    check(timeline.applyThrough(*first,bank) && bank.get(17)==.25, "repeated token changed state");
    check(!timeline.applyThrough(*second+1,bank) && bank.get(17)==.25, "unknown future token changed state");
    check(timeline.applyThrough(*second,bank) && bank.get(17)==.5, "second token not applied");
    check(!timeline.applyThrough(*first,bank) && bank.get(17)==.5, "stale token rewound state");
    timeline.reclaim();
    std::array<ParameterTimeline::Value,1> invalid{{{999,.5}}};
    check(!timeline.publish(bank,invalid) && timeline.published()==*second, "invalid packet advanced timeline");
    std::array<ParameterTimeline::Value,1> value{{{17,.1}}};
    for(unsigned index=0;index<ParameterTimeline::packetCount;++index)check(bool(timeline.publish(bank,value)), "bounded packet admission");
    const auto full=timeline.published();
    check(!timeline.publish(bank,value) && timeline.published()==full, "queue overflow discarded older packets");
    check(timeline.applyThrough(full,bank), "skipped token failed to adopt preceding packets");
    timeline.reclaim();
    cell_test::failAfter=0; const auto failed=timeline.publish(bank,value); cell_test::failAfter=-1;
    check(!failed && timeline.published()==full, "allocation failure partially admitted update");
    check(bool(timeline.publish(bank,value)), "producer did not recover after backpressure");
}
void largeCatalog() {
    std::vector<cell::Parameter> descriptors;
    std::vector<ParameterTimeline::Value> values;
    for(unsigned n=0;n<54434;++n) {
        descriptors.push_back({n+1,"parameter"+std::to_string(n),"Parameter "+std::to_string(n),"",0,1,0});
        values.push_back({n+1,double(n%101)/100});
    }
    cell::ParameterBank bank(descriptors); ParameterTimeline timeline;
    auto token=timeline.publish(bank,values); check(bool(token),"whole Studio-sized catalog admission");
    check(*token>1&&!timeline.applyThrough(*token-1,bank)&&bank.get(2)==0,"unissued intermediate token exposed a partial catalog");
    cell_test::allocations=cell_test::deletions=0;cell_test::audio=true;
    const auto applied=timeline.applyThrough(*token,bank);
    cell_test::audio=false;
    check(applied && cell_test::allocations==0 && cell_test::deletions==0,"large-catalog adoption allocated");
    for(const auto& [id,value]:values)check(bank.get(id)==value,"whole-catalog value missing");
    timeline.reclaim();
    // Reject the entire invalid batch before allocating or exposing its prefix.
    values.back().first=UINT32_MAX;
    check(!timeline.publish(bank,values) && timeline.published()==*token,"invalid large batch partially admitted");
}
void contention() {
    cell::ParameterBank bank(descriptors); ParameterTimeline timeline;
    std::atomic<std::uint64_t> target{};std::atomic<bool> done{},failed{};
    constexpr unsigned rounds=10000;
    std::thread audio([&] {
        std::uint64_t seen{};
        while(!done.load(std::memory_order_acquire)||seen<target.load(std::memory_order_acquire)) {
            auto next=target.load(std::memory_order_acquire);
            if(next!=seen) { if(!timeline.applyThrough(next,bank))failed=true; seen=next; }
            else std::this_thread::yield();
        }
    });
    for(unsigned n=1;n<=rounds;++n) {
        const std::array<ParameterTimeline::Value,2> values{{{17,double(n%101)/100},{18,double((n+1)%101)/100}}};
        std::optional<std::uint64_t> token;
        while(!(token=timeline.publish(bank,values)))std::this_thread::yield();
        target.store(*token,std::memory_order_release);
    }
    done.store(true,std::memory_order_release);audio.join();timeline.reclaim();
    check(!failed && bank.get(17)==double(rounds%101)/100,"concurrent timeline lost final value");
}
}
int main() {
    try { timingAndAdmission();largeCatalog();contention();std::cout<<"PASS: timed parameter handoff, whole catalog, backpressure, allocation failure and contention\n"; }
    catch(const std::exception& error){std::cerr<<error.what()<<'\n';return 1;}
}
