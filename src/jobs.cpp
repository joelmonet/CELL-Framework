#include <cell/jobs.hpp>
#include <algorithm>
#include <array>
#include <cmath>
#include <condition_variable>
#include <deque>
#include <mutex>
#include <stdexcept>
#include <thread>
#include <unordered_map>
#include <vector>

namespace cell::jobs {
namespace detail {
struct Owner { std::atomic<std::uint64_t> generation{1}; std::atomic<bool> alive{true}; };
struct State {
    Token token;
    std::atomic<bool> cancelled{};
    std::atomic<double> progress{};
    mutable std::mutex mutex;
    std::condition_variable completed;
    Result result;
};
static thread_local State* currentWork{};
static bool cancelled(const State& state) noexcept { return state.cancelled.load() || !state.token.valid(); }
static void finish(const std::shared_ptr<State>& state, Result result) {
    if (cancelled(*state)) result = {Status::cancelled, state->progress.load(), {}, {}};
    if (result.status != Status::succeeded && result.status != Status::cancelled) result.status = Status::failed;
    if (result.status == Status::succeeded) state->progress.store(1);
    result.progress = state->progress.load();
    if (result.message.size() > 2048) result.message.resize(2048);
    { std::lock_guard lock(state->mutex); state->result = std::move(result); }
    state->completed.notify_all();
}
}
bool Token::valid() const noexcept {
    const auto state = owner_.lock();
    return state && state->alive.load() && state->generation.load() == generation_;
}
Owner::Owner() : state_(std::make_shared<detail::Owner>()) {}
Owner::~Owner() { state_->alive.store(false); }
Token Owner::token() const noexcept { Token value; value.owner_ = state_; value.generation_ = state_->generation.load(); return value; }
Token Owner::next() noexcept {
    // Do not wrap and make a decades-old token valid again.
    auto value = state_->generation.load();
    while (value != UINT64_MAX && !state_->generation.compare_exchange_weak(value, value + 1)) {}
    if (value == UINT64_MAX) state_->alive.store(false);
    return token();
}
void Owner::cancel() noexcept { (void)next(); }
Context::Context(std::shared_ptr<detail::State> state) : state_(std::move(state)) {}
bool Context::cancelled() const noexcept { return detail::cancelled(*state_); }
void Context::progress(double value) noexcept {
    if (!std::isfinite(value)) return;
    value = std::clamp(value, 0., 1.);
    auto previous = state_->progress.load();
    while (value > previous && !state_->progress.compare_exchange_weak(previous, value)) {}
}
Job::Job(std::shared_ptr<detail::State> state) : state_(std::move(state)) {}
Job::~Job() { cancel(); }
Job::Job(Job&& other) noexcept : state_(std::move(other.state_)) {}
Job& Job::operator=(Job&& other) noexcept {
    if (this != &other) { cancel(); state_ = std::move(other.state_); }
    return *this;
}
Result Job::poll() const {
    if (!state_) return {Status::cancelled, 0, {}, {}};
    std::lock_guard lock(state_->mutex);
    if (detail::cancelled(*state_)) return {Status::cancelled, state_->progress.load(), {}, {}};
    auto result = state_->result;
    result.progress = state_->progress.load();
    return result;
}
bool Job::settled() const {
    if (!state_) return true;
    std::lock_guard lock(state_->mutex);
    return state_->result.finished();
}
bool Job::isCurrentWorker() const noexcept { return state_ && detail::currentWork == state_.get(); }
void Job::cancel() noexcept { if (state_) state_->cancelled.store(true); }
void Job::wait() const {
    if (!state_) return;
    std::unique_lock lock(state_->mutex);
    state_->completed.wait(lock, [&] { return state_->result.finished(); });
}
struct Pool::Impl : std::enable_shared_from_this<Pool::Impl> {
    struct Item { std::shared_ptr<detail::State> state; Work work; };
    std::mutex mutex;
    std::condition_variable wake, workersStopped;
    bool joiningWorkers{};
    unsigned liveWorkers{}, workerCount{};
    inline static thread_local Impl* currentPool{};
    struct Registry {
        std::mutex mutex;
        std::vector<std::shared_ptr<Impl>> pools;
        bool quiesced{};
        ~Registry() { for (const auto& pool : pools) pool->stop(); }
    };
    static Registry& registry() { static Registry value; return value; }
    std::array<std::deque<Item>, 3> queues;
    std::vector<std::thread> workers;
    std::unordered_map<detail::State*, std::weak_ptr<detail::State>> active;
    unsigned capacity;
    std::size_t admitted{};
    std::uint64_t turns{};
    bool stopping{};
    explicit Impl(unsigned limit) : capacity(limit) {}
    void start(unsigned count) {
        workerCount = count;
        try {
            for (unsigned i = 0; i < count; ++i) {
                { std::lock_guard lock(mutex); ++liveWorkers; }
                try {
                    workers.emplace_back([self = shared_from_this()] {
                        currentPool = self.get();
                        self->run();
                        { std::lock_guard lock(self->mutex); --self->liveWorkers; }
                        self->workersStopped.notify_all();
                    });
                } catch (...) {
                    { std::lock_guard lock(mutex); --liveWorkers; }
                    workersStopped.notify_all();
                    throw;
                }
            }
        } catch (...) { stop(); throw; }
    }
    void stop() {
        const bool fromWorker = currentPool == this;
        std::vector<std::thread> joining;
        {
            std::unique_lock lock(mutex);
            stopping = true;
            for (auto& entry : active)
                if (auto state = entry.second.lock()) state->cancelled.store(true);
            wake.notify_all();
            // The module registry retains this Impl and its join handles when
            // its final Pool owner dies in a job. Do not detach: a mere "work
            // returned" flag precedes CRT/TLS and Windows DLL_THREAD_DETACH.
            if (fromWorker) return;
            workersStopped.wait(lock, [&] { return !joiningWorkers; });
            joiningWorkers = true;
            joining.swap(workers);
        }
        for (auto& thread : joining) if (thread.joinable()) thread.join();
        {
            std::lock_guard lock(mutex);
            joiningWorkers = false;
        }
        workersStopped.notify_all();
    }
    void resume() {
        {
            std::lock_guard lock(mutex);
            if (!stopping) return;
            if (liveWorkers || admitted) throw std::logic_error("Workers have not quiesced");
            stopping = false;
        }
        start(workerCount);
    }
    void run() noexcept {
        for (;;) {
            Item item;
            {
                std::unique_lock lock(mutex);
                wake.wait(lock, [&] { return stopping || !queues[0].empty() || !queues[1].empty() || !queues[2].empty(); });
                if (queues[0].empty() && queues[1].empty() && queues[2].empty()) return;
                // Every eighth dispatch gives background work a turn. Normal
                // work gets every fourth, even with continuous UI admission.
                unsigned index = 0;
                ++turns;
                if (turns % 8 == 0 && !queues[2].empty()) index = 2;
                else if (turns % 4 == 0 && !queues[1].empty()) index = 1;
                else while (index < 2 && queues[index].empty()) ++index;
                item = std::move(queues[index].front()); queues[index].pop_front();
                { std::lock_guard stateLock(item.state->mutex); item.state->result.status = Status::running; }
            }
            detail::currentWork = item.state.get();
            Result result;
            try {
                Context context(item.state);
                result = context.cancelled() ? Result{Status::cancelled, 0, {}, {}} : item.work(context);
            } catch (const std::exception& error) { result = Result::failure(error.what()); }
              catch (...) { result = Result::failure("Background operation failed"); }
            // Release captured readers and graphs before a completion barrier
            // lets their owner close host services. This runs off audio.
            item.work = {};
            detail::currentWork = nullptr;
            detail::finish(item.state, std::move(result));
            { std::lock_guard lock(mutex); --admitted;
              active.erase(item.state.get()); }
        }
    }
};
Pool::Pool(unsigned workers, unsigned capacity) {
    if (!workers) throw std::invalid_argument("A job pool needs at least one worker");
    auto& registry = Impl::registry();
    std::lock_guard lock(registry.mutex);
    if (registry.quiesced) throw std::runtime_error("Module workers are shutting down");
    impl_ = std::make_shared<Impl>(capacity);
    std::erase_if(registry.pools, [](const auto& pool) { return pool.use_count() == 1 && pool->workers.empty(); });
    registry.pools.push_back(impl_);
    impl_->start(workers);
}
Pool::~Pool() { impl_->stop(); }
Job Pool::submit(Token token, Work work, Priority priority) {
    if (!token.valid() || !work || unsigned(priority) > 2) return {};
    auto state = std::make_shared<detail::State>(); state->token = std::move(token);
    { std::lock_guard lock(impl_->mutex);
      if (impl_->stopping || (impl_->capacity && impl_->admitted == impl_->capacity)) return {};
      impl_->active.emplace(state.get(), state);
      try { impl_->queues[unsigned(priority)].push_back({state, std::move(work)}); }
      catch (...) { impl_->active.erase(state.get()); throw; }
      ++impl_->admitted; }
    impl_->wake.notify_one();
    return Job(std::move(state));
}
void quiesceModulePools() {
    if (Pool::Impl::currentPool) throw std::logic_error("A worker cannot unload its module");
    auto& registry = Pool::Impl::registry();
    { std::lock_guard lock(registry.mutex); registry.quiesced = true; }
    // No allocation in module teardown. New pool admission is closed, and the
    // registry keeps worker-retired pools/OS join handles alive until joined.
    for (std::size_t index = 0;; ++index) {
        std::shared_ptr<Pool::Impl> pool;
        {
            std::lock_guard lock(registry.mutex);
            if (index == registry.pools.size()) break;
            pool = registry.pools[index];
        }
        pool->stop();
    }
}
void resumeModulePools() {
    auto& registry = Pool::Impl::registry();
    std::unique_lock lock(registry.mutex);
    std::erase_if(registry.pools, [](const auto& pool) { return pool.use_count() == 1 && pool->workers.empty(); });
    try {
        for (const auto& pool : registry.pools) pool->resume();
        registry.quiesced = false;
    } catch (...) {
        // Partial reentry must not leave earlier pools alive when the format
        // rejects initialization and the host immediately unloads its library.
        lock.unlock();
        quiesceModulePools();
        throw;
    }
}
std::shared_ptr<Pool> Pool::shared() {
    static std::mutex mutex;
    static std::weak_ptr<Pool> previous;
    std::lock_guard lock(mutex);
    auto pool = previous.lock();
    if (!pool) { pool = std::make_shared<Pool>(); previous = pool; }
    return pool;
}
}
