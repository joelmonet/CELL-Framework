// Original Cell Framework implementation; see LICENSE.
#pragma once
#include <cell/streams.hpp>
#include <cstring>
#include <limits>
namespace cell {
// Borrows immutable memory. Its owner must outlive the stream.
class MemoryInputStream final : public InputStream {
    std::span<const std::byte> data_; std::int64_t position_{};
public:
    explicit MemoryInputStream(std::span<const std::byte> data) : data_(data) {
        if (data.size() > std::uint64_t(INT64_MAX)) throw std::length_error("Input exceeds stream addressing");
    }
    MemoryInputStream(const void* data, std::size_t size)
        : MemoryInputStream(std::span(static_cast<const std::byte*>(data), data || !size ? size : throw std::invalid_argument("Missing input memory"))) {}
    int read(void* output, int count) override {
        if (count <= 0 || position_ >= getTotalLength()) return 0;
        if (!output) throw std::invalid_argument("Missing stream destination");
        const auto amount = std::min<std::size_t>(std::size_t(count), data_.size() - std::size_t(position_));
        std::memmove(output, data_.data() + std::size_t(position_), amount); position_ += std::int64_t(amount); return int(amount);
    }
    std::int64_t getPosition() const override { return position_; }
    std::int64_t getTotalLength() const override { return std::int64_t(data_.size()); }
    bool setPosition(std::int64_t value) override { if (value < 0) return false; position_ = value; return true; }
    bool isExhausted() const override { return position_ >= getTotalLength(); }
};
class MemoryOutputStream final : public OutputStream {
    std::vector<std::byte> bytes_; std::int64_t position_{};
public:
    bool write(const void* source, std::size_t count) override {
        if ((!source && count) || count > std::uint64_t(INT64_MAX - position_) ||
            std::uint64_t(position_) + count > std::numeric_limits<std::size_t>::max()) return false;
        if (!count) return true;
        const auto address = reinterpret_cast<std::uintptr_t>(source), base = reinterpret_cast<std::uintptr_t>(bytes_.data());
        const bool alias = !bytes_.empty() && address >= base && address - base < bytes_.size();
        const auto offset = alias ? std::size_t(address - base) : 0;
        if (alias && count > bytes_.size() - offset) return false;
        const auto end = std::size_t(position_) + count;
        if (end > bytes_.size()) bytes_.resize(end);
        std::memmove(bytes_.data() + std::size_t(position_), alias ? bytes_.data() + offset : source, count); position_ += std::int64_t(count); return true;
    }
    void flush() override {}
    std::int64_t getPosition() const override { return position_; }
    bool setPosition(std::int64_t value) override { if (value < 0) return false; position_ = value; return true; }
    const void* getData() const noexcept { return bytes_.data(); }
    std::size_t getDataSize() const noexcept { return bytes_.size(); }
    std::span<const std::byte> bytes() const noexcept { return bytes_; }
    ByteBlock getMemoryBlock() const { return ByteBlock(bytes_); }
    ByteBlock takeBlock() && { position_ = 0; return ByteBlock(std::move(bytes_)); }
};
}
