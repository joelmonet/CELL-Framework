// Original Cell Framework implementation; see LICENSE.
#include <cell/widgets/input.hpp>
#include <chrono>
#include <cstdio>
#include <queue>
#include <thread>

namespace cell::ui::widgets {
namespace detail {
InputState&inputState()noexcept{static thread_local InputState state;return state;}
}
ModifierKeys ModifierKeys::getCurrentModifiers()noexcept{return detail::inputState().modifiers;}
bool KeyPress::isKeyCurrentlyDown(int key)noexcept{if(key>='A'&&key<='Z')key+='a'-'A';return detail::inputState().down.contains(key);}
struct Timer::State {
    Timer*owner{};std::uint64_t generation{};std::chrono::milliseconds period{};
    bool running{};std::function<void()>oneShot;
};
struct TimerRegistry {
    using Clock=std::chrono::steady_clock;
    struct Item {Clock::time_point due;std::uint64_t serial,generation;std::shared_ptr<Timer::State>state;};
    struct Later {bool operator()(const Item&a,const Item&b)const{return a.due!=b.due?a.due>b.due:a.serial>b.serial;}};
    std::priority_queue<Item,std::vector<Item>,Later>queue;
    std::uint64_t serial{};bool dispatching{};
    static TimerRegistry&get(){static thread_local TimerRegistry registry;return registry;}
    void push(const std::shared_ptr<Timer::State>&s){queue.push({Clock::now()+s->period,++serial,s->generation,s});}
    void dispatch(){
        if(dispatching)return;
        dispatching=true;struct Finish{bool&flag;~Finish(){flag=false;}}finish{dispatching};
        const auto budget=Clock::now()+std::chrono::milliseconds(3);
        while(!queue.empty()&&queue.top().due<=Clock::now()){
            auto item=queue.top();queue.pop();auto&state=*item.state;
            if(state.running&&state.generation==item.generation){
                try{if(state.owner)state.owner->timerCallback();else if(state.oneShot){auto fn=std::move(state.oneShot);state.running=false;fn();}}
                catch(const std::exception&e){std::fprintf(stderr,"Cell UI timer: %s\n",e.what());}
                catch(...){std::fputs("Cell UI timer failed\n",stderr);}
                if(state.running&&state.owner&&state.generation==item.generation)push(item.state);
            }
            if(Clock::now()>=budget)break;
        }
    }
};
Timer::Timer():state_(std::make_shared<State>()){state_->owner=this;}
Timer::~Timer(){stopTimer();state_->owner=nullptr;}
void Timer::startTimer(int milliseconds){if(milliseconds<=0){stopTimer();return;}state_->period=std::chrono::milliseconds(milliseconds);state_->running=true;++state_->generation;TimerRegistry::get().push(state_);}
void Timer::startTimerHz(int hz){startTimer(hz>0?std::max(1,1000/hz):0);}
void Timer::stopTimer(){state_->running=false;++state_->generation;}
bool Timer::isTimerRunning()const{return state_->running;}
void Timer::callAfterDelay(int milliseconds,std::function<void()>fn){if(!fn)return;auto state=std::make_shared<State>();state->period=std::chrono::milliseconds(std::max(0,milliseconds));state->running=true;state->oneShot=std::move(fn);TimerRegistry::get().push(state);}
void Timer::dispatchPending(){TimerRegistry::get().dispatch();}
}
