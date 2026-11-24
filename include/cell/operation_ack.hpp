#pragma once
#include <cell/commands.hpp>
#include <memory>
#include <atomic>

namespace cell {
// One admitted non-audio operation. A transport's successful send is separate
// from the target's receipt. Replies may arrive on a different host thread;
// consume/finalize runs on the GUI thread that owns the controller/editor.
class OperationAck {
public:
    struct Request { std::uint64_t token{}; std::shared_ptr<const std::vector<std::byte>> bytes; };
    struct Receipt { Request request; bool accepted{}; };
    Request begin(std::span<const std::byte> bytes, std::size_t maximum) {
        if (bytes.empty() || bytes.size() > maximum) return {};
        auto payload = std::make_shared<const std::vector<std::byte>>(bytes.begin(), bytes.end());
        std::lock_guard lock(mutex_);
        if (status_ == RestoreStatus::pending) return {};
        static std::atomic<std::uint64_t> serial{std::uint64_t(std::chrono::steady_clock::now().time_since_epoch().count()) & 0x3fffffffffffffffull};
        auto previous = serial.load();
        while (previous < std::uint64_t(INT64_MAX) && !serial.compare_exchange_weak(previous, previous + 1)) {}
        if (previous >= std::uint64_t(INT64_MAX)) return {};
        request_ = {previous + 1, std::move(payload)}; status_ = RestoreStatus::pending;
        received_ = consumed_ = accepted_ = false; return request_;
    }
    bool acknowledge(std::uint64_t token, bool accepted) {
        std::lock_guard lock(mutex_);
        if (!token || token != request_.token || status_ != RestoreStatus::pending) return false;
        if (received_) return accepted_ == accepted;
        received_ = true; accepted_ = accepted; return true;
    }
    std::optional<Receipt> consume(std::uint64_t token) {
        std::lock_guard lock(mutex_);
        if (!token || token != request_.token || status_ != RestoreStatus::pending || !received_ || consumed_) return {};
        consumed_ = true; return Receipt{request_, accepted_};
    }
    bool finish(std::uint64_t token, bool applied) {
        std::lock_guard lock(mutex_);
        if (!token || token != request_.token || status_ != RestoreStatus::pending) return false;
        status_ = applied ? RestoreStatus::succeeded : RestoreStatus::failed; request_.bytes.reset(); return true;
    }
    RestoreStatus status(std::uint64_t token) const {
        std::lock_guard lock(mutex_); return token && token == request_.token ? status_ : RestoreStatus::unknown;
    }
private:
    mutable std::mutex mutex_;
    Request request_;
    RestoreStatus status_ = RestoreStatus::unknown;
    bool received_{}, consumed_{}, accepted_{};
};
}
