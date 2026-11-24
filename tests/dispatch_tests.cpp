// Original Cell Framework implementation; see LICENSE.
#include <cell/dispatch.hpp>
#include <atomic>
#include <thread>
#include <vector>
#include <stdexcept>
#include <iostream>
int main(){try{using D=cell::MainThreadDispatcher;const auto ui=std::this_thread::get_id();std::atomic<int>calls{};std::vector<std::thread>threads;for(int t=0;t<8;++t)threads.emplace_back([&]{for(int n=0;n<1000;++n)D::post([&]{if(std::this_thread::get_id()!=ui)throw std::runtime_error("Wrong dispatch thread");++calls;});});for(auto&t:threads)t.join();if(D::pending()!=8000)throw std::runtime_error("Message admission lost work");while(D::pending())D::dispatch();if(calls!=8000)throw std::runtime_error("Thread-safe messages lost or duplicated");
 std::vector<int>order;D::post([&]{order.push_back(1);D::post([&]{order.push_back(3);});D::dispatch();order.push_back(2);});while(D::pending())D::dispatch();if(order!=std::vector<int>{1,2,3})throw std::runtime_error("Reentrant dispatch corrupted FIFO order");
 D::post([]{throw std::runtime_error("Expected callback failure");});D::post([&]{++calls;});bool threw=false;try{D::dispatch(std::chrono::seconds(1));}catch(const std::runtime_error&){threw=true;}if(!threw||calls!=8001||D::pending())throw std::runtime_error("Callback failure dropped unrelated messages");int errors=0;D::setExceptionHandler([&](std::exception_ptr e){if(e)++errors;});D::post([]{throw std::runtime_error("Handled callback failure");});D::dispatch();D::setExceptionHandler({});if(errors!=1)throw std::runtime_error("Error delivery missing");std::cout<<"PASS native main-thread dispatcher: 8000 concurrent messages, FIFO, reentrancy, budgeted draining and error isolation\n";return 0;
 }catch(const std::exception&e){std::cerr<<e.what()<<'\n';return 1;}}
