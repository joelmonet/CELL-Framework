#include <cell/state_transfer.hpp>
#include <algorithm>
#include <chrono>
#include <map>
#include <mutex>
#include <stdexcept>

namespace cell::state {
namespace {
enum class Operation : unsigned { capture = 1, chunk = 2, release = 3 };
enum class Reply : unsigned { waiting, ready, failed };
std::atomic<std::uint64_t> serial{1};
std::uint64_t nextSerial() {
    auto previous = serial.load();
    while (previous != UINT64_MAX && !serial.compare_exchange_weak(previous, previous + 1)) {}
    if (previous == UINT64_MAX) throw std::overflow_error("snapshot identifiers exhausted");
    return previous;
}
std::uint64_t digest(std::span<const std::byte> data) {
    std::uint64_t value = 14695981039346656037ull;
    for (auto byte : data) value = (value ^ std::to_integer<unsigned char>(byte)) * 1099511628211ull;
    return value;
}
struct Request { Operation operation{}; std::uint64_t nonce{}, owner{}, offset{}; };
std::vector<std::byte> encode(Request request) {
    ByteWriter w; w.u32(transferMagic); w.u32(1); w.u32(unsigned(request.operation));
    w.u64(request.nonce); w.u64(request.owner); w.u64(request.offset); return std::move(w.bytes);
}
bool decode(std::span<const std::byte> bytes, Request& value) {
    ByteReader r(bytes); unsigned magic{}, version{}, operation{};
    if (!r.u32(magic) || magic != transferMagic || !r.u32(version) || version != 1 ||
        !r.u32(operation) || operation < 1 || operation > 3 || !r.u64(value.nonce) || !value.nonce ||
        !r.u64(value.owner) || !r.u64(value.offset) || r.remaining()) return false;
    value.operation = Operation(operation);
    return operation == 1 ? !value.owner && !value.offset : value.owner && (operation != 3 || !value.offset);
}
struct Snapshot { std::vector<std::byte> bytes; std::uint64_t digest{}; };
std::vector<std::byte> reply(Request request, Reply status, std::uint64_t owner, const Snapshot* state = nullptr) {
    ByteWriter w; w.u32(transferMagic); w.u32(1); w.u32(unsigned(request.operation)); w.u32(unsigned(status));
    w.u64(request.nonce); w.u64(owner); w.u64(state ? state->bytes.size() : 0); w.u64(request.offset); w.u64(state ? state->digest : 0);
    std::size_t count = 0;
    if (state && request.operation == Operation::chunk && request.offset < state->bytes.size()) count = std::min(transferChunkBytes, state->bytes.size() - std::size_t(request.offset));
    w.u32(unsigned(count));
    if (count) w.bytes.insert(w.bytes.end(), state->bytes.begin() + request.offset, state->bytes.begin() + request.offset + count);
    return std::move(w.bytes);
}
}
bool isTransfer(std::span<const std::byte> bytes) noexcept {
    ByteReader r(bytes); unsigned magic{}; return r.u32(magic) && magic == transferMagic;
}
struct SnapshotServer::Impl {
    using Clock = std::chrono::steady_clock;
    struct Entry { jobs::Job job; std::shared_ptr<const jobs::Payload> data; Clock::time_point used; };
    Capture capture;
    std::shared_ptr<jobs::Pool> pool;
    jobs::Owner requests;
    std::uint64_t owner = nextSerial();
    std::mutex mutex;
    std::map<std::uint64_t, Entry> entries;
    std::vector<jobs::Job> retired;
    Impl(Capture work, std::shared_ptr<jobs::Pool> workers) : capture(std::move(work)), pool(std::move(workers)) {}
    void retire(Entry& entry) { if (entry.job && !entry.job.settled()) { entry.job.cancel(); retired.push_back(std::move(entry.job)); } }
    void collect() {
        std::erase_if(retired, [](const auto& job) { return job.settled(); });
        const auto now = Clock::now();
        for (auto it = entries.begin(); it != entries.end();) {
            if (now - it->second.used > std::chrono::seconds(30)) { retire(it->second); it = entries.erase(it); } else ++it;
        }
    }
    jobs::Job submit() {
        return pool->submit(requests.token(), [work = capture](jobs::Context& context) {
            if (context.cancelled()) return jobs::Result{jobs::Status::cancelled, 0, {}, {}};
            auto data = work();
            // A changing graph/resource returns no candidate and is retried on
            // the next poll. It never publishes a partially loaded document.
            if (!data) return jobs::Result::success(std::optional<Snapshot>{});
            if (data->empty() || data->size() > transferMaximumBytes) return jobs::Result::failure("Snapshot exceeds transfer limit");
            Snapshot state; state.digest = digest(*data); state.bytes = std::move(*data);
            return jobs::Result::success(std::optional<Snapshot>{std::move(state)});
        }, jobs::Priority::interactive);
    }
};
SnapshotServer::SnapshotServer(Capture capture, std::shared_ptr<jobs::Pool> pool) {
    if (!capture || !pool) throw std::invalid_argument("snapshot callbacks");
    impl_ = std::make_unique<Impl>(std::move(capture), std::move(pool));
}
SnapshotServer::~SnapshotServer() {
    impl_->requests.cancel();
    // Callback borrows its Engine. Keep that owner alive until work returns.
    for (auto& [nonce, entry] : impl_->entries) { (void)nonce; entry.job.cancel(); entry.job.wait(); }
    for (auto& job : impl_->retired) { job.cancel(); job.wait(); }
}
std::vector<std::byte> SnapshotServer::query(std::span<const std::byte> bytes) {
    Request request; if (!decode(bytes, request)) return {};
    std::lock_guard lock(impl_->mutex); impl_->collect();
    auto it = impl_->entries.find(request.nonce);
    if (request.operation == Operation::capture) {
        if (it == impl_->entries.end()) {
            // Two 128 MiB snapshots maximum, including cancelled work still
            // running. Admission never evicts another editor's live lease.
            if (impl_->entries.size() + impl_->retired.size() >= 2) return reply(request, Reply::failed, impl_->owner);
            auto job = impl_->submit(); if (!job) return reply(request, Reply::failed, impl_->owner);
            it = impl_->entries.emplace(request.nonce, Impl::Entry{std::move(job), {}, Impl::Clock::now()}).first;
        }
        it->second.used = Impl::Clock::now();
        if (!it->second.data) {
            const auto result = it->second.job.poll();
            if (!result.finished()) return reply(request, Reply::waiting, impl_->owner);
            const auto* data = result.get<std::optional<Snapshot>>();
            if (!data) return reply(request, Reply::failed, impl_->owner);
            if (!*data) { it->second.job = impl_->submit(); return reply(request, it->second.job ? Reply::waiting : Reply::failed, impl_->owner); }
            it->second.data = result.payload;
        }
        const auto* value = static_cast<const jobs::Value<std::optional<Snapshot>>*>(it->second.data.get());
        return reply(request, Reply::ready, impl_->owner, &*value->data);
    }
    if (request.owner != impl_->owner || it == impl_->entries.end()) return reply(request, Reply::failed, impl_->owner);
    it->second.used = Impl::Clock::now();
    if (request.operation == Operation::release) { impl_->retire(it->second); impl_->entries.erase(it); return reply(request, Reply::ready, impl_->owner); }
    if (!it->second.data) return reply(request, Reply::failed, impl_->owner);
    const auto* value = static_cast<const jobs::Value<std::optional<Snapshot>>*>(it->second.data.get());
    if (request.offset >= value->data->bytes.size()) return reply(request, Reply::failed, impl_->owner);
    return reply(request, Reply::ready, impl_->owner, &*value->data);
}
void SnapshotClient::start() {
    if (status_ == Status::waiting) throw std::logic_error("cancel the previous snapshot before starting another");
    nonce_ = nextSerial(); owner_ = leaseOwner_ = total_ = digest_ = 0; runningDigest_ = 14695981039346656037ull; bytes_.clear(); status_ = Status::waiting;
}
SnapshotClient::Status SnapshotClient::poll(const Query& query) {
    if (status_ != Status::waiting || !query) return status_;
    const auto fail = [&] { cancel(query); status_ = Status::failed; return status_; };
    try {
    for (unsigned turn = 0; turn < 4; ++turn) {
        const Request request{owner_ ? Operation::chunk : Operation::capture, nonce_, owner_, bytes_.size()};
        const auto data = query(encode(request), false); if (data.empty()) return status_;
        ByteReader r(data); unsigned magic{}, version{}, operation{}, status{}, length{}; std::uint64_t nonce{}, owner{}, total{}, offset{}, checksum{};
        std::span<const std::byte> chunk;
        if (!r.u32(magic) || magic != transferMagic || !r.u32(version) || version != 1 || !r.u32(operation) || operation != unsigned(request.operation) ||
            !r.u32(status) || status > 2 || !r.u64(nonce) || nonce != nonce_ || !r.u64(owner) || !owner || !r.u64(total) || total > transferMaximumBytes ||
            !r.u64(offset) || offset != request.offset || !r.u64(checksum) || !r.u32(length) || length > transferChunkBytes || !r.raw(length, chunk) || r.remaining()) {
            return fail();
        }
        if (leaseOwner_ && leaseOwner_ != owner) return fail();
        leaseOwner_ = owner;
        if (status == unsigned(Reply::failed)) return fail();
        if (status == unsigned(Reply::waiting)) { if (owner_ || total || length) return fail(); return status_; }
        if (!total || (owner_ && (owner != owner_ || total != total_ || checksum != digest_))) { return fail(); }
        if (!owner_) {
            if (length) { return fail(); }
            owner_ = owner; total_ = total; digest_ = checksum; bytes_.reserve(std::size_t(total));
        } else {
            if (!length || length != std::min<std::uint64_t>(transferChunkBytes, total - bytes_.size())) { return fail(); }
            for (const auto byte : chunk) runningDigest_ = (runningDigest_ ^ std::to_integer<unsigned char>(byte)) * 1099511628211ull;
            bytes_.insert(bytes_.end(), chunk.begin(), chunk.end());
            if (bytes_.size() == total_) {
                if (runningDigest_ != digest_) { return fail(); }
                (void)query(encode({Operation::release, nonce_, owner_, 0}), false);
                status_ = Status::ready; return status_;
            }
        }
    }
    return status_;
    } catch (...) { return fail(); }
}
void SnapshotClient::cancel(const Query& query) noexcept {
    const auto owner = owner_ ? owner_ : leaseOwner_;
    bytes_.clear(); owner_ = leaseOwner_ = total_ = digest_ = runningDigest_ = 0; status_ = Status::idle;
    if (owner && query) try { (void)query(encode({Operation::release, nonce_, owner, 0}), false); } catch (...) {}
}
std::vector<std::byte> SnapshotClient::take() {
    if (status_ != Status::ready) return {};
    status_ = Status::idle; owner_ = leaseOwner_ = total_ = digest_ = 0; return std::move(bytes_);
}
}
