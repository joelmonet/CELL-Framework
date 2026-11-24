#include <cell/read_ahead.hpp>
#include <atomic>
#include <condition_variable>
#include <future>
#include <iostream>
#include <mutex>
#include <stdexcept>
#include <thread>
using namespace std::chrono_literals;
namespace {
void check(bool value, const char* message) { if (!value) throw std::runtime_error(message); }
struct Client final : cell::ReadAheadClient {
    std::atomic<unsigned> calls{};
    std::chrono::milliseconds delay{0};
    bool fail{};
    std::chrono::milliseconds readAheadSlice() override {
        ++calls;
        if (fail) throw std::runtime_error("test reader failure");
        return delay;
    }
};
struct Blocking final : cell::ReadAheadClient {
    std::promise<void> entered, release;
    std::shared_future<void> gate = release.get_future().share();
    std::atomic<unsigned> calls{};
    std::chrono::milliseconds readAheadSlice() override {
        ++calls; entered.set_value(); gate.wait(); return 1h;
    }
};
}
int main() {
    cell::ReadAheadService service;
    Client busy, occasional, broken;
    occasional.delay = 1ms; broken.fail = true;
    service.add(busy); service.add(occasional); service.add(broken);
    const auto deadline = std::chrono::steady_clock::now() + 3s;
    while (occasional.calls < 8 || broken.calls < 2) {
        check(std::chrono::steady_clock::now() < deadline, "Busy or throwing reader starved another source");
        std::this_thread::sleep_for(1ms);
    }
    bool duplicate = false;
    try { service.add(busy); } catch (const std::invalid_argument&) { duplicate = true; }
    check(duplicate, "Duplicate reader registration accepted");
    service.remove(busy); service.remove(occasional); service.remove(broken);
    const auto count = busy.calls.load();
    std::this_thread::sleep_for(10ms);
    check(busy.calls == count && count > 0, "Removed reader was still called");
    service.remove(busy);

    Blocking blocked;
    auto entered = blocked.entered.get_future();
    service.add(blocked);
    check(entered.wait_for(3s) == std::future_status::ready, "Worker did not start blocked source");
    auto removed = std::async(std::launch::async, [&] { service.remove(blocked); });
    const auto premature = removed.wait_for(20ms) == std::future_status::ready;
    blocked.release.set_value();
    check(removed.wait_for(3s) == std::future_status::ready, "Reader teardown did not join its callback");
    removed.get();
    check(!premature && blocked.calls == 1, "Reader teardown was not a callback lifetime barrier");

    Client sleeping;
    sleeping.delay = 1h;
    { cell::ReadAheadService temporary; temporary.add(sleeping); temporary.remove(sleeping); }
    std::cout << "PASS fair read-ahead, exception containment, duplicate rejection and joined reader teardown\n";
}
