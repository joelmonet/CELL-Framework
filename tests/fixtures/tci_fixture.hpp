// Independently generated test data, containing no library audio.
#pragma once
#include <algorithm>
#include <bit>
#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <vector>

namespace cell_test {
inline void tciU32(std::vector<std::byte> &out, std::uint32_t value) {
  for (unsigned n = 0; n < 4; ++n)
    out.push_back(std::byte((value >> (8 * n)) & 255));
}
inline void tciRecord(std::vector<std::byte> &out, unsigned tag,
                      std::span<const std::byte> payload) {
  tciU32(out, tag);
  tciU32(out, std::uint32_t(payload.size()));
  out.insert(out.end(), payload.begin(), payload.end());
}
inline void tciScalar(std::vector<std::byte> &out, unsigned tag,
                      std::uint32_t value) {
  std::vector<std::byte> bytes;
  tciU32(bytes, value);
  tciRecord(out, tag, bytes);
}
struct TciSample {
  unsigned channels = 1;
  std::string name = "own-synthetic";
  std::vector<std::int32_t> scalars;
};
inline std::vector<std::byte> tciBytes(std::span<const TciSample> samples) {
  const std::string magic = "TRIGGER COMPRESSED INSTRUMENT";
  std::vector<std::byte> out(64);
  for (unsigned n = 0; n < magic.size(); ++n)
    out[n] = std::byte(magic[n]);
  tciScalar(out, 1, 4);
  tciScalar(out, 20, 48000);
  tciScalar(out, 4, unsigned(samples.size()));
  for (const auto &sample : samples) {
    std::vector<bool> bits;
    const auto append = [&](std::uint32_t value, unsigned width) {
      for (unsigned n = width; n > 0; --n)
        bits.push_back(bool((value >> (n - 1)) & 1));
    };
    for (std::size_t first = 0; first + 1 < sample.scalars.size();
         first += 201) {
      const auto end = std::min(first + 201, sample.scalars.size() - 1);
      std::uint32_t magnitude{};
      for (auto n = first; n < end; ++n)
        magnitude = std::max(
            magnitude, std::uint32_t(sample.scalars[n] < 0
                                         ? -std::int64_t(sample.scalars[n])
                                         : sample.scalars[n]));
      const unsigned width = std::bit_width(magnitude) + 1;
      append(width, 8);
      for (auto n = first; n < end; ++n) {
        const auto value = sample.scalars[n];
        append(value < 0 ? (std::uint32_t(1) << (width - 1)) |
                               std::uint32_t(-std::int64_t(value))
                         : std::uint32_t(value),
               width);
      }
    }
    std::vector<std::byte> payload{std::byte(sample.channels)};
    tciU32(payload, unsigned(bits.size()));
    tciU32(payload, unsigned(sample.scalars.size()));
    const auto offset = payload.size();
    payload.resize(offset + bits.size() / 8 + 1);
    for (std::size_t n = 0; n < bits.size(); ++n)
      if (bits[n])
        payload[offset + n / 8] |= std::byte(128u >> (n % 8));
    for (unsigned char c : sample.name)
      payload.push_back(std::byte(c));
    tciRecord(out, 5, payload);
  }
  // Preserve unknown metadata without interpreting it as an audio pointer.
  tciScalar(out, 789, 12345);
  tciRecord(out, 3, {});
  return out;
}
} // namespace cell_test
