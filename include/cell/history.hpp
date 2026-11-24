#pragma once

#include <cell/core.hpp>
#include <functional>
#include <memory>
#include <string>
#include <unordered_map>

namespace cell::history {

// GUI/control-thread state history. Archives own their bytes, share unchanged
// 64 KiB pages, and never retain a processor, editor, or external file handle.
class Archive {
public:
    Archive() = default;
    std::size_t size() const noexcept { return size_; }
    bool empty() const noexcept { return !size_; }
    std::vector<std::byte> bytes() const;
    bool read(std::size_t offset, std::span<std::byte>) const noexcept;
    Archive slice(std::size_t offset, std::size_t count) const;
    bool operator==(const Archive&) const noexcept;
private:
    friend class Store;
    friend class Timeline;
    struct Page { std::vector<std::byte> bytes; };
    using Pages = std::vector<std::shared_ptr<const Page>>;
    const Pages& pages() const noexcept { static const Pages empty; return pages_ ? *pages_ : empty; }
    std::shared_ptr<const Pages> pages_;
    std::size_t size_{};
};

class Store {
public:
    static constexpr std::size_t pageBytes = 65536;
    explicit Store(std::size_t maximumArchiveBytes = 128u * 1024 * 1024);
    ~Store();
    Store(Store&&) noexcept;
    Store& operator=(Store&&) noexcept;
    Store(const Store&) = delete;
    Store& operator=(const Store&) = delete;
    Archive capture(std::span<const std::byte>);
    Archive join(std::span<const Archive>) const;
    void collect();
private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

class Timeline {
public:
    struct Limits { std::size_t entries = 64, bytes = 256u * 1024 * 1024; };
    struct Entry { Archive state; std::string label; };
    explicit Timeline(Limits);
    Timeline() : Timeline(Limits{}) {}
    // Failed admission/restore leaves the cursor, redo branch and comparison
    // untouched. Hosts with asynchronous restore use prepare/complete below.
    bool reset(Archive, std::string label = "Initial state");
    bool record(Archive, std::string label);
    bool canUndo() const noexcept { return !entries_.empty() && cursor_ > 0; }
    bool canRedo() const noexcept { return cursor_ + 1 < entries_.size(); }
    const Entry* current() const noexcept;
    const Entry* undoTarget() const noexcept;
    const Entry* redoTarget() const noexcept;
    bool undo(const std::function<bool(const Archive&)>&);
    bool redo(const std::function<bool(const Archive&)>&);
    std::uint64_t prepare(bool redo);
    const Archive* pending(std::uint64_t ticket) const noexcept;
    bool complete(std::uint64_t ticket, bool restored) noexcept;
    void cancel() noexcept;
    bool setComparison(Archive);
    const Archive& comparison() const noexcept { return comparison_; }
    std::size_t residentBytes() const;
    std::size_t count() const noexcept { return entries_.size(); }
private:
    struct Accounting { std::unordered_map<const Archive::Page*, std::size_t> pages; std::size_t bytes{}; };
    static void account(Accounting&, const Archive&, bool add);
    static void account(Accounting&, const Entry&, bool add);
    Limits limits_;
    std::vector<Entry> entries_;
    Archive comparison_;
    std::size_t cursor_{}, destination_{};
    std::uint64_t serial_{}, ticket_{};
    Accounting accounting_;
};
}
