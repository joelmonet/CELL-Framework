#include <cell/history.hpp>
#include <algorithm>
#include <stdexcept>
#include <unordered_map>
#include <unordered_set>

namespace cell::history {
std::vector<std::byte> Archive::bytes() const {
    std::vector<std::byte> result; result.reserve(size_);
    for (const auto& page : pages()) result.insert(result.end(), page->bytes.begin(), page->bytes.end());
    return result;
}
bool Archive::operator==(const Archive& other) const noexcept {
    if (size_ != other.size_) return false;
    std::size_t a = 0, b = 0, x = 0, y = 0;
    while (a < pages().size() && b < other.pages().size()) {
        const auto& left = pages()[a]; const auto& right = other.pages()[b];
        const auto count = std::min(left->bytes.size() - x, right->bytes.size() - y);
        if (!(left == right && x == y) && !std::equal(left->bytes.begin() + x, left->bytes.begin() + x + count, right->bytes.begin() + y)) return false;
        x += count; y += count;
        if (x == left->bytes.size()) { ++a; x = 0; }
        if (y == right->bytes.size()) { ++b; y = 0; }
    }
    return true;
}
bool Archive::read(std::size_t offset, std::span<std::byte> output) const noexcept {
    if (offset > size_ || output.size() > size_ - offset) return false;
    for (const auto& page : pages()) {
        if (offset >= page->bytes.size()) { offset -= page->bytes.size(); continue; }
        const auto count = std::min(output.size(), page->bytes.size() - offset);
        std::copy_n(page->bytes.begin() + offset, count, output.begin()); output = output.subspan(count); offset = 0;
        if (output.empty()) break;
    }
    return true;
}
Archive Archive::slice(std::size_t offset, std::size_t count) const {
    if (offset > size_ || count > size_ - offset) throw std::out_of_range("archive slice");
    Archive result; result.size_ = count; auto pages = std::make_shared<Pages>();
    for (const auto& page : this->pages()) {
        if (!count) break;
        if (offset >= page->bytes.size()) { offset -= page->bytes.size(); continue; }
        const auto copied = std::min(count, page->bytes.size() - offset);
        if (!offset && copied == page->bytes.size()) pages->push_back(page);
        else { auto part = std::make_shared<Page>(); part->bytes.assign(page->bytes.begin() + offset, page->bytes.begin() + offset + copied); pages->push_back(std::move(part)); }
        count -= copied; offset = 0;
    }
    result.pages_ = std::move(pages); return result;
}
struct Store::Impl {
    std::unordered_map<std::uint64_t, std::vector<std::weak_ptr<const Archive::Page>>> pages;
    std::size_t maximum; unsigned captures{};
};
Store::Store(std::size_t maximum) : impl_(std::make_unique<Impl>()) {
    if (!maximum || maximum > 1024u * 1024 * 1024) throw std::invalid_argument("archive limit");
    impl_->maximum = maximum;
}
Store::~Store() = default;
Store::Store(Store&&) noexcept = default;
Store& Store::operator=(Store&&) noexcept = default;
void Store::collect() {
    for (auto it = impl_->pages.begin(); it != impl_->pages.end();) {
        std::erase_if(it->second, [](const auto& weak) { return weak.expired(); });
        if (it->second.empty()) it = impl_->pages.erase(it); else ++it;
    }
}
Archive Store::capture(std::span<const std::byte> bytes) {
    if (bytes.size() > impl_->maximum) throw std::length_error("state exceeds archive limit");
    if (++impl_->captures % 32 == 0) collect();
    Archive result; result.size_ = bytes.size(); auto pages = std::make_shared<Archive::Pages>();
    pages->reserve((bytes.size() + pageBytes - 1) / pageBytes);
    for (std::size_t offset = 0; offset < bytes.size(); offset += pageBytes) {
        const auto input = bytes.subspan(offset, std::min(pageBytes, bytes.size() - offset));
        std::uint64_t hash = 14695981039346656037ull;
        for (const auto value : input) hash = (hash ^ std::to_integer<unsigned char>(value)) * 1099511628211ull;
        auto& candidates = impl_->pages[hash];
        std::shared_ptr<const Archive::Page> match;
        for (const auto& weak : candidates) if (auto page = weak.lock())
            if (page->bytes.size() == input.size() && std::equal(input.begin(), input.end(), page->bytes.begin())) { match = std::move(page); break; }
        // Hashes only accelerate lookup. Exact bytes decide identity.
        if (!match) {
            auto page = std::make_shared<Archive::Page>(); page->bytes.assign(input.begin(), input.end());
            match = page; candidates.push_back(match);
        }
        pages->push_back(std::move(match));
    }
    result.pages_ = std::move(pages); return result;
}
Archive Store::join(std::span<const Archive> parts) const {
    Archive result; auto pages = std::make_shared<Archive::Pages>();
    std::size_t pageCount = 0; for (const auto& part : parts) pageCount += part.pages().size(); pages->reserve(pageCount);
    for (const auto& part : parts) {
        if (part.size_ > impl_->maximum - result.size_) throw std::length_error("joined state exceeds archive limit");
        result.size_ += part.size_; pages->insert(pages->end(), part.pages().begin(), part.pages().end());
    }
    result.pages_ = std::move(pages); return result;
}
Timeline::Timeline(Limits limits) : limits_(limits) {
    if (!limits.entries || limits.entries > 4096 || !limits.bytes) throw std::invalid_argument("history limits");
}
void Timeline::account(Accounting& budget, const Archive& state, bool add) {
    const auto overhead = state.pages().size() * sizeof(std::shared_ptr<const Archive::Page>);
    if (add) budget.bytes += overhead; else budget.bytes -= overhead;
    for (const auto& page : state.pages()) {
        if (add) {
            if (++budget.pages[page.get()] == 1) budget.bytes += sizeof(Archive::Page) + page->bytes.size();
        } else {
            auto it = budget.pages.find(page.get());
            if (it != budget.pages.end() && !--it->second) { budget.bytes -= sizeof(Archive::Page) + page->bytes.size(); budget.pages.erase(it); }
        }
    }
}
void Timeline::account(Accounting& budget, const Entry& entry, bool add) {
    account(budget, entry.state, add);
    const auto bytes = sizeof(Entry) + entry.label.size(); if (add) budget.bytes += bytes; else budget.bytes -= bytes;
}
bool Timeline::reset(Archive state, std::string label) {
    std::vector<Entry> candidate{{std::move(state), std::move(label)}}; Accounting budget;
    account(budget, candidate.front(), true);
    if (budget.bytes > limits_.bytes) return false;
    entries_.swap(candidate); accounting_ = std::move(budget); comparison_ = {}; cursor_ = 0; cancel(); return true;
}
bool Timeline::record(Archive state, std::string label) {
    if (entries_.empty()) return reset(std::move(state), std::move(label));
    if (entries_[cursor_].state == state) return true;
    // Archive copies share their immutable page tables. The budget updates
    // only changed entries; it does not rescan every sample in every undo step.
    std::vector<Entry> candidate(entries_.begin(), entries_.begin() + cursor_ + 1);
    auto budget = accounting_;
    for (std::size_t i = cursor_ + 1; i < entries_.size(); ++i) account(budget, entries_[i], false);
    candidate.push_back({std::move(state), std::move(label)}); account(budget, candidate.back(), true);
    while (candidate.size() > 1 && (candidate.size() > limits_.entries || budget.bytes > limits_.bytes)) {
        account(budget, candidate.front(), false); candidate.erase(candidate.begin());
    }
    if (budget.bytes > limits_.bytes) return false;
    entries_.swap(candidate); accounting_ = std::move(budget); cursor_ = entries_.size() - 1; cancel(); return true;
}
const Timeline::Entry* Timeline::current() const noexcept { return entries_.empty() ? nullptr : &entries_[cursor_]; }
const Timeline::Entry* Timeline::undoTarget() const noexcept { return canUndo() ? &entries_[cursor_ - 1] : nullptr; }
const Timeline::Entry* Timeline::redoTarget() const noexcept { return canRedo() ? &entries_[cursor_ + 1] : nullptr; }
std::uint64_t Timeline::prepare(bool redo) {
    if (ticket_ || !(redo ? canRedo() : canUndo()) || serial_ == UINT64_MAX) return 0;
    destination_ = redo ? cursor_ + 1 : cursor_ - 1; ticket_ = ++serial_; return ticket_;
}
const Archive* Timeline::pending(std::uint64_t ticket) const noexcept {
    return ticket && ticket == ticket_ ? &entries_[destination_].state : nullptr;
}
bool Timeline::complete(std::uint64_t ticket, bool restored) noexcept {
    if (!ticket || ticket != ticket_) return false;
    if (restored) cursor_ = destination_;
    ticket_ = 0; return restored;
}
void Timeline::cancel() noexcept { ticket_ = 0; }
bool Timeline::undo(const std::function<bool(const Archive&)>& restore) {
    const auto ticket = prepare(false); if (!ticket) return false;
    const auto state = *pending(ticket);
    try { return complete(ticket, restore(state)); } catch (...) { complete(ticket, false); throw; }
}
bool Timeline::redo(const std::function<bool(const Archive&)>& restore) {
    const auto ticket = prepare(true); if (!ticket) return false;
    const auto state = *pending(ticket);
    try { return complete(ticket, restore(state)); } catch (...) { complete(ticket, false); throw; }
}
bool Timeline::setComparison(Archive state) {
    auto budget = accounting_; account(budget, comparison_, false); account(budget, state, true);
    if (budget.bytes > limits_.bytes) return false;
    comparison_ = std::move(state); accounting_ = std::move(budget); return true;
}
std::size_t Timeline::residentBytes() const { return accounting_.bytes; }
}
