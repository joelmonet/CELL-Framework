#pragma once
#include <string_view>

#include <array>
#include <atomic>
#include <bit>
#include <cstddef>
#include <cstdint>
#include <span>
#include <type_traits>
#include <vector>

namespace cell {

// One producer and one consumer; neither side waits or allocates.
template<class T, std::size_t Capacity>
class SpscQueue {
    static_assert(Capacity > 1 && std::is_trivially_copyable_v<T>);
public:
    std::size_t freeCapacity() const noexcept {
        const auto write = write_.load(std::memory_order_relaxed);
        const auto read = read_.load(std::memory_order_acquire);
        return (read + Capacity - write - 1) % Capacity;
    }
    // Consumer-side snapshot of the items published by the producer.
    std::size_t available() const noexcept {
        const auto read = read_.load(std::memory_order_relaxed);
        const auto write = write_.load(std::memory_order_acquire);
        return (write + Capacity - read) % Capacity;
    }
    bool pushBatch(std::span<const T> values) noexcept {
        if (values.size() > freeCapacity()) return false;
        auto write = write_.load(std::memory_order_relaxed);
        for (const auto& value : values) { data_[write] = value; write = (write + 1) % Capacity; }
        write_.store(write, std::memory_order_release);
        return true;
    }
    bool push(const T& value) noexcept {
        const auto write = write_.load(std::memory_order_relaxed);
        const auto next = (write + 1) % Capacity;
        if (next == read_.load(std::memory_order_acquire)) return false;
        data_[write] = value;
        write_.store(next, std::memory_order_release);
        return true;
    }
    bool pop(T& value) noexcept {
        const auto read = read_.load(std::memory_order_relaxed);
        if (read == write_.load(std::memory_order_acquire)) return false;
        value = data_[read];
        read_.store((read + 1) % Capacity, std::memory_order_release);
        return true;
    }
private:
    std::array<T, Capacity> data_{};
    alignas(64) std::atomic<std::size_t> write_{};
    alignas(64) std::atomic<std::size_t> read_{};
};

// State uses explicit little-endian fields, never native structs or padding.
class ByteWriter {
public:
    void u32(std::uint32_t value) {
        for (unsigned shift = 0; shift < 32; shift += 8)
            bytes.push_back(static_cast<std::byte>(value >> shift));
    }
    void u64(std::uint64_t value) {
        for (unsigned shift = 0; shift < 64; shift += 8)
            bytes.push_back(static_cast<std::byte>(value >> shift));
    }
    void f64(double value) { u64(std::bit_cast<std::uint64_t>(value)); }
    std::vector<std::byte> bytes;
};

class ByteReader {
public:
    explicit ByteReader(std::span<const std::byte> input) : input_(input) {}
    bool u32(std::uint32_t& value) noexcept {
        if (remaining() < 4) return false;
        value = 0;
        for (unsigned shift = 0; shift < 32; shift += 8)
            value |= std::uint32_t(std::to_integer<unsigned char>(input_[offset_++])) << shift;
        return true;
    }
    bool u64(std::uint64_t& value) noexcept {
        if (remaining() < 8) return false;
        value = 0;
        for (unsigned shift = 0; shift < 64; shift += 8)
            value |= std::uint64_t(std::to_integer<unsigned char>(input_[offset_++])) << shift;
        return true;
    }
    bool f64(double& value) noexcept {
        std::uint64_t raw{};
        if (!u64(raw)) return false;
        value = std::bit_cast<double>(raw);
        return true;
    }
    bool raw(std::size_t size,std::span<const std::byte>& value) noexcept {
        if(size>remaining())return false;
        value=input_.subspan(offset_,size);offset_+=size;return true;
    }
    std::size_t remaining() const noexcept { return input_.size() - offset_; }
private:
    std::span<const std::byte> input_;
    std::size_t offset_{};
};

constexpr std::uint32_t stableId(const char* text) noexcept {
    std::uint32_t result = 2166136261u;
    while (*text) result = (result ^ static_cast<unsigned char>(*text++)) * 16777619u;
    return result & 0x7fffffffu;
}

constexpr std::uint32_t stableId(std::string_view text) noexcept {
    std::uint32_t hash = 2166136261u;
    for (const auto c : text) hash = (hash ^ static_cast<unsigned char>(c)) * 16777619u;
    return hash & 0x7fffffffu;
}

} // namespace cell
