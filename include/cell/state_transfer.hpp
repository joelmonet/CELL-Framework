#pragma once

#include <cell/commands.hpp>
#include <cell/core.hpp>
#include <cell/jobs.hpp>
#include <optional>

namespace cell::state {
inline constexpr std::uint32_t transferMagic = 0x54535343;
inline constexpr std::size_t transferChunkBytes = 192u * 1024;
inline constexpr std::size_t transferMaximumBytes = 128u * 1024 * 1024;
bool isTransfer(std::span<const std::byte>) noexcept;

// An immutable processor-side snapshot is leased to a client and downloaded in
// bounded messages. VST3's controller must use this instead of saving its local
// parameter cache, which does not own the live processing component's assets.
// Capture callbacks are non-realtime and must support concurrent state saves.
class SnapshotServer {
public:
    using Capture = std::function<std::optional<std::vector<std::byte>>() >;
    explicit SnapshotServer(Capture, std::shared_ptr<jobs::Pool> = jobs::Pool::shared());
    ~SnapshotServer();
    SnapshotServer(const SnapshotServer&) = delete;
    SnapshotServer& operator=(const SnapshotServer&) = delete;
    std::vector<std::byte> query(std::span<const std::byte>);
private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

class SnapshotClient {
public:
    using Query = std::function<std::vector<std::byte>(std::span<const std::byte>, bool)>;
    enum class Status { idle, waiting, ready, failed };
    void start();
    Status poll(const Query&); // bounded to four completed chunks per GUI tick
    void cancel(const Query& = {}) noexcept;
    Status status() const noexcept { return status_; }
    std::vector<std::byte> take();
private:
    std::uint64_t nonce_{}, owner_{}, leaseOwner_{}, total_{}, digest_{}, runningDigest_{};
    Status status_ = Status::idle;
    std::vector<std::byte> bytes_;
};
}
