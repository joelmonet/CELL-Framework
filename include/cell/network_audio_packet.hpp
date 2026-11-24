#pragma once

#include <cmath>
#include <optional>
#include <utility>

#include <cell/network_audio.hpp>

namespace cell::network_audio {

// One datagram carries one packet for an already negotiated stream.
inline constexpr std::size_t maxPacketBytes = 1200;
namespace detail {
inline constexpr std::uint32_t pcmV1 = 0x31414e43; // "CNA1" on the wire
inline constexpr std::size_t headerBytes = 24;
static_assert(sizeof(float) == 4 && std::numeric_limits<float>::is_iec559);

template<std::size_t Channels>
constexpr bool validPacket(std::uint32_t rate, std::uint64_t first, std::size_t frames) noexcept {
    static_assert(Channels > 0 && Channels <= 32);
    return rate >= 8000 && rate <= 384000 && frames > 0 &&
        frames <= (maxPacketBytes - headerBytes) / sizeof(float) / Channels &&
        frames <= std::numeric_limits<std::uint64_t>::max() - first;
}
} // namespace detail

template<std::size_t Channels>
std::vector<std::byte> encodePacket(std::uint32_t rate, std::uint64_t firstSample,
                                    std::span<const Frame<Channels>> frames) {
    if (!detail::validPacket<Channels>(rate, firstSample, frames.size())) return {};
    ByteWriter writer;
    writer.bytes.reserve(detail::headerBytes + frames.size_bytes());
    writer.u32(detail::pcmV1);
    writer.u32(rate);
    writer.u32(Channels);
    writer.u32(static_cast<std::uint32_t>(frames.size()));
    writer.u64(firstSample);
    for (const auto& frame : frames) {
        for (float sample : frame) {
            if (!std::isfinite(sample)) return {};
            writer.u32(std::bit_cast<std::uint32_t>(sample));
        }
    }
    return std::move(writer.bytes);
}

struct PacketPosition {
    std::uint64_t firstSample;
    std::size_t frames;
};

// Decode into caller-owned scratch storage. Use the output only on success.
template<std::size_t Channels>
std::optional<PacketPosition> decodePacket(std::span<const std::byte> bytes,
                                            std::uint32_t expectedRate,
                                            std::span<Frame<Channels>> output) noexcept {
    ByteReader reader(bytes);
    std::uint32_t magic{}, rate{}, channels{}, frames{};
    std::uint64_t firstSample{};
    if (!reader.u32(magic) || !reader.u32(rate) || !reader.u32(channels) ||
        !reader.u32(frames) || !reader.u64(firstSample) ||
        magic != detail::pcmV1 || rate != expectedRate || channels != Channels ||
        !detail::validPacket<Channels>(rate, firstSample, frames) || frames > output.size() ||
        reader.remaining() != frames * Channels * sizeof(float)) return std::nullopt;
    for (auto& frame : output.first(frames)) {
        for (auto& sample : frame) {
            std::uint32_t bits{};
            if (!reader.u32(bits)) return std::nullopt;
            sample = std::bit_cast<float>(bits);
            if (!std::isfinite(sample)) return std::nullopt;
        }
    }
    return PacketPosition{firstSample, frames};
}

} // namespace cell::network_audio
