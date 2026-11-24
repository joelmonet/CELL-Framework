// Original Cell Framework implementation; see LICENSE.
#include <cell/dispatch.hpp>
#include <deque>
#include <map>
#include <mutex>
#include <thread>
namespace cell {
namespace {
std::recursive_mutex control;
std::mutex mutex;
std::deque<std::function<void()>>queue,controlQueue;
std::multimap<std::chrono::steady_clock::time_point,std::function<void()>>delayed;
std::thread::id owner;
std::function<void(std::exception_ptr)>errorHandler;
thread_local bool dispatching;
}
ControlScope::ControlScope():lock_(control){}
ControlScope::ControlScope(std::try_to_lock_t):lock_(control,std::try_to_lock){}
bool MainThreadDispatcher::post(std::function<void()>fn){if(fn){std::lock_guard lock(mutex);queue.push_back(std::move(fn));}return true;}
bool MainThreadDispatcher::postControl(std::function<void()>fn){if(fn){std::lock_guard lock(mutex);controlQueue.push_back(std::move(fn));}return true;}
bool MainThreadDispatcher::postAfter(std::chrono::milliseconds delay,std::function<void()>fn){if(delay.count()<=0)return post(std::move(fn));if(fn){std::lock_guard lock(mutex);delayed.emplace(std::chrono::steady_clock::now()+delay,std::move(fn));}return true;}
void MainThreadDispatcher::bindCurrentThread(){std::lock_guard lock(mutex);if(owner!=std::thread::id{}&&owner!=std::this_thread::get_id())throw std::logic_error("Cell dispatcher already belongs to another thread");owner=std::this_thread::get_id();}
bool MainThreadDispatcher::isOwnerThread(){std::lock_guard lock(mutex);return owner==std::thread::id{}||owner==std::this_thread::get_id();}
std::size_t MainThreadDispatcher::pending(){std::lock_guard lock(mutex);return queue.size()+controlQueue.size()+delayed.size();}
void MainThreadDispatcher::setExceptionHandler(std::function<void(std::exception_ptr)>handler){std::lock_guard lock(mutex);errorHandler=std::move(handler);}
void MainThreadDispatcher::dispatch(std::chrono::nanoseconds budget){dispatchImpl(budget,false);}
void MainThreadDispatcher::dispatchControl(std::chrono::nanoseconds budget){dispatchImpl(budget,true);}
void MainThreadDispatcher::dispatchImpl(std::chrono::nanoseconds budget,bool dataOnly){
 ControlScope scope;
 if(dispatching||budget.count()<=0)return;
 if(!dataOnly&&!isOwnerThread())throw std::logic_error("Cell GUI dispatch on a foreign thread");
 dispatching=true;struct Exit{~Exit(){dispatching=false;}}exit;
 const auto end=std::chrono::steady_clock::now()+budget;
 std::exception_ptr failure;
 bool preferControl=true;
 do{
  std::function<void()>fn;std::function<void(std::exception_ptr)>handler;
  {
   std::lock_guard lock(mutex);
   if(!dataOnly){const auto now=std::chrono::steady_clock::now();while(!delayed.empty()&&delayed.begin()->first<=now){queue.push_back(std::move(delayed.begin()->second));delayed.erase(delayed.begin());}}
   auto*next=(dataOnly||queue.empty()||(preferControl&&!controlQueue.empty()))?&controlQueue:&queue;
   if(next->empty())break;
   fn=std::move(next->front());next->pop_front();handler=errorHandler;preferControl=!preferControl;
  }
  try{fn();}catch(...){if(handler){try{handler(std::current_exception());}catch(...){if(!failure)failure=std::current_exception();}}else if(!failure)failure=std::current_exception();}
 }while(std::chrono::steady_clock::now()<end);
 if(failure)std::rethrow_exception(failure);
}
}
