#pragma once

#include <atomic>
#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <utility>

namespace cell::jobs {

// All APIs are non-realtime. Workers never call an editor or a host callback.
// Consumers poll an owning result and decide on their own thread how to use it.
enum class Status { queued, running, succeeded, failed, cancelled };
enum class Priority { interactive, normal, background };
struct Payload { virtual ~Payload() = default; };
template<class T> struct Value final : Payload {
    explicit Value(T value) : data(std::move(value)) {}
    T data;
};
struct Result {
    Status status = Status::queued;
    double progress{};
    std::string message;
    std::shared_ptr<const Payload> payload;
    bool finished() const noexcept {
        return status == Status::succeeded || status == Status::failed || status == Status::cancelled;
    }
    template<class T> const T* get() const noexcept {
        const auto* value = dynamic_cast<const Value<T>*>(payload.get());
        return value ? &value->data : nullptr;
    }
    template<class T> static Result success(T value) {
        return {Status::succeeded, 1, {}, std::make_shared<Value<T>>(std::move(value))};
    }
    static Result failure(std::string message) {
        return {Status::failed, 0, std::move(message), {}};
    }
};

namespace detail { struct Owner; struct State; }
class Token {
public:
    bool valid() const noexcept;
private:
    friend class Owner;
    std::weak_ptr<detail::Owner> owner_;
    std::uint64_t generation_{};
};
// A new generation cancels every prior request. Destruction also invalidates
// completed results, so a closed/replaced editor cannot receive an old payload.
class Owner {
public:
    Owner();
    ~Owner();
    Owner(const Owner&) = delete;
    Owner& operator=(const Owner&) = delete;
    Token token() const noexcept;
    Token next() noexcept;
    void cancel() noexcept;
private:
    std::shared_ptr<detail::Owner> state_;
};

class Context {
public:
    bool cancelled() const noexcept;
    void progress(double fraction) noexcept;
private:
    friend class Pool;
    explicit Context(std::shared_ptr<detail::State>);
    std::shared_ptr<detail::State> state_;
};

class Job {
public:
    Job() = default;
    ~Job();
    Job(Job&&) noexcept;
    Job& operator=(Job&&) noexcept;
    Job(const Job&) = delete;
    Job& operator=(const Job&) = delete;
    explicit operator bool() const noexcept { return bool(state_); }
    Result poll() const;
    bool settled() const; // underlying work has returned, including cancellation
    bool isCurrentWorker() const noexcept; // non-audio ownership teardown
    void cancel() noexcept;
    // Cooperative completion barrier for owners of work that borrows a service.
    // Never use from an audio callback, GUI animation, or a pool worker.
    void wait() const;
private:
    friend class Pool;
    explicit Job(std::shared_ptr<detail::State>);
    std::shared_ptr<detail::State> state_;
};

class Pool {
public:
    using Work = std::function<Result(Context&)>;
    // Zero capacity lets admission follow available memory. Finite capacities
    // are for fixed-resource consumers, never a project track count.
    explicit Pool(unsigned workers = 2, unsigned capacity = 128);
    ~Pool();
    Pool(const Pool&) = delete;
    Pool& operator=(const Pool&) = delete;
    // A full queue rejects admission; it never evicts a different user's work.
    Job submit(Token, Work, Priority = Priority::normal);
    static std::shared_ptr<Pool> shared();
private:
    friend void quiesceModulePools();
    friend void resumeModulePools();
    struct Impl;
    std::shared_ptr<Impl> impl_;
};
// Format entry/exit hooks, after all instances have retired their work. Call
// outside the OS loader lock and never from audio or a worker. Quiescence cancels
// queued/running jobs and joins every module worker, including retired workers
// whose final Pool owner was released by a job. Existing idle pools can restart
// on a subsequent module entry without requiring a physical DLL unload.
void quiesceModulePools();
void resumeModulePools();
}
