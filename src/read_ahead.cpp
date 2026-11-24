#include <cell/read_ahead.hpp>
#include <algorithm>
#include <condition_variable>
#include <cstdint>
#include <mutex>
#include <stdexcept>
#include <thread>
#include <vector>

namespace cell {
struct ReadAheadService::Impl {
    using Clock = std::chrono::steady_clock;
    struct Entry {
        ReadAheadClient* client;
        Clock::time_point due;
        std::uint64_t id;
    };
    std::mutex mutex;
    std::condition_variable changed;
    std::vector<Entry> entries;
    ReadAheadClient* active{};
    std::uint64_t nextId{}, lastServed{};
    bool stopping{};
    std::thread worker;

    Impl() : worker([this] { run(); }) {}
    ~Impl() {
        {
            std::lock_guard lock(mutex);
            stopping = true;
        }
        changed.notify_all();
        worker.join();
    }
    void run() {
        std::unique_lock lock(mutex);
        while (!stopping) {
            const auto now = Clock::now();
            auto selected = entries.end();
            auto earliest = Clock::time_point::max();
            // Ticket order gives every due client a turn, including when a
            // busy client returns zero repeatedly or the registry changes.
            for (auto it = entries.begin(); it != entries.end(); ++it) {
                earliest = std::min(earliest, it->due);
                if (it->due > now) continue;
                const bool after = it->id > lastServed;
                if (selected == entries.end() ||
                    (after && selected->id <= lastServed) ||
                    (after == (selected->id > lastServed) && it->id < selected->id))
                    selected = it;
            }
            if (selected == entries.end()) {
                if (earliest == Clock::time_point::max()) changed.wait(lock);
                else changed.wait_until(lock, earliest);
                continue;
            }
            auto* client = selected->client;
            const auto id = selected->id;
            active = client;
            lastServed = id;
            lock.unlock();
            auto delay = std::chrono::milliseconds{100};
            try { delay = client->readAheadSlice(); }
            catch (...) { /* A bad source cannot terminate every media reader. */ }
            // Bound arithmetic before adding to the clock. Negative delays
            // mean immediate work, rather than a wrapped/overflowed deadline.
            delay = std::clamp(delay, std::chrono::milliseconds{0},
                               std::chrono::milliseconds{86400000});
            lock.lock();
            for (auto& entry : entries)
                if (entry.id == id) { entry.due = Clock::now() + delay; break; }
            active = nullptr;
            changed.notify_all();
        }
    }
};

ReadAheadService::ReadAheadService() : impl_(std::make_unique<Impl>()) {}
ReadAheadService::~ReadAheadService() = default;
void ReadAheadService::add(ReadAheadClient& client) {
    {
        std::lock_guard lock(impl_->mutex);
        if (std::any_of(impl_->entries.begin(), impl_->entries.end(),
                        [&](const auto& entry) { return entry.client == &client; }))
            throw std::invalid_argument("Read-ahead client already registered");
        impl_->entries.push_back({&client, Impl::Clock::now(), ++impl_->nextId});
    }
    impl_->changed.notify_all();
}
void ReadAheadService::remove(ReadAheadClient& client) noexcept {
    std::unique_lock lock(impl_->mutex);
    std::erase_if(impl_->entries, [&](const auto& entry) { return entry.client == &client; });
    impl_->changed.notify_all();
    impl_->changed.wait(lock, [&] { return impl_->active != &client; });
}
}
