// Original Cell Framework implementation; see LICENSE.
#include <cell/jobs.hpp>
#include <atomic>
#include <chrono>
#include <iostream>
#include <memory>
#include <stdexcept>
#include <thread>

namespace {
void need(bool value, const char* message) {
    if (!value) throw std::runtime_error(message);
}
template<class Predicate> void until(Predicate predicate) {
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
    while (!predicate()) {
        need(std::chrono::steady_clock::now() < deadline, "Module worker test timed out");
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
}
struct ThreadExit {
    std::atomic<unsigned>* completed{};
    ~ThreadExit() { if (completed) ++*completed; }
};
}

int main() {
    try {
        std::atomic<unsigned> entered{}, exited{};
        cell::jobs::Owner owner;
        cell::jobs::Pool first(2, 0), second(3, 0);
        const auto work = [&](cell::jobs::Context& context) {
            thread_local ThreadExit atExit{&exited};
            ++entered;
            while (!context.cancelled())
                std::this_thread::sleep_for(std::chrono::milliseconds(1));
            return cell::jobs::Result{cell::jobs::Status::cancelled, 0, {}, {}};
        };
        auto a = first.submit(owner.token(), work);
        auto b = second.submit(owner.token(), work);
        until([&] { return entered == 2; });
        cell::jobs::quiesceModulePools();
        need(a.settled() && b.settled() && exited == 2,
             "Module shutdown returned before worker CRT/TLS destruction");
        need(!first.submit(owner.token(), work), "Quiesced pool admitted new work");
        bool rejected = false;
        try { cell::jobs::Pool closed(1, 0); }
        catch (const std::runtime_error&) { rejected = true; }
        need(rejected, "Module shutdown allowed a new worker pool");
        cell::jobs::resumeModulePools();
        auto resumed = first.submit(owner.token(), [](auto&) {
            return cell::jobs::Result{cell::jobs::Status::succeeded, 1, {}, {}};
        });
        resumed.wait();
        need(resumed.poll().status == cell::jobs::Status::succeeded,
             "Reentered module could not reuse its existing shared pool");

        // A job may destroy the last Pool owner. Keep its actual OS join handle
        // until the lifecycle thread joins it, including thread-local cleanup.
        std::atomic<bool> retired{}, allowRetirement{};
        auto self = std::make_shared<cell::jobs::Pool>(2, 0);
        auto lifetime = self;
        auto retiring = self->submit(owner.token(), [lifetime = std::move(lifetime), &retired,
                &allowRetirement, &exited](auto&) mutable {
            thread_local ThreadExit atExit{&exited};
            while (!allowRetirement)
                std::this_thread::sleep_for(std::chrono::milliseconds(1));
            lifetime.reset();
            retired = true;
            return cell::jobs::Result{cell::jobs::Status::succeeded, 1, {}, {}};
        });
        self.reset();
        allowRetirement = true;
        until([&] { return retired.load(); });
        cell::jobs::quiesceModulePools();
        need(retiring.settled() && exited == 3,
             "Worker-owned pool retirement lost its full thread-exit barrier");
        cell::jobs::quiesceModulePools();
        cell::jobs::resumeModulePools();
        std::cout << "PASS native module workers: cancellation, full OS/TLS exit before unload, admission closure, idempotent teardown, reentry and worker-owned pool retirement\n";
    } catch (const std::exception& error) {
        std::cerr << "FAIL native module workers: " << error.what() << '\n';
        return 1;
    }
}
