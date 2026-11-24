// SPDX-License-Identifier: LicenseRef-Sound-Cells-Commercial
#pragma once
#include <cell/gog.hpp>
#include <bit>
#include <cell/core.hpp>
#include <cell/files.hpp>

namespace cell_test {
struct GogSample {
  unsigned channels = 1, bits = 16, rate = 48000, frames = 1024, group = 0;
  float amplitude = .2f;
  bool enabled = true;
  std::string name = "Own fixture";
};
inline std::vector<std::byte> gogBytes(std::span<const GogSample> samples,
                                       std::span<const float> levels) {
  std::vector<std::byte> result(cell::gog::headerBytes);
  auto u16 = [&](unsigned at, unsigned value) {
    result.at(at) = std::byte(value);
    result.at(at + 1) = std::byte(value >> 8);
  };
  auto u32 = [&](unsigned at, unsigned value) {
    u16(at, value);
    u16(at + 2, value >> 16);
  };
  constexpr std::string_view magic = "Gog_v3.5";
  for (unsigned n = 0; n < magic.size(); ++n)
    result[n] = std::byte(magic[n]);
  u16(366, unsigned(samples.size()));
  u16(368, unsigned(levels.size()));
  for (unsigned n = 0; n < levels.size(); ++n)
    u32(370 + n * 4, std::bit_cast<unsigned>(levels[n]));
  for (unsigned n = 0; n < samples.size(); ++n) {
    const auto &sample = samples[n];
    u32(26210 + n * 4, sample.frames * sample.channels);
    u32(29666 + n * 4, sample.rate);
    u16(28898 + n * 2, sample.group + 1);
    result[28514 + n] = std::byte(sample.enabled);
    result[33506 + n] = std::byte(sample.channels - 1);
    result[33890 + n] = std::byte(sample.bits);
    const auto size = std::min<std::size_t>(30, sample.name.size());
    result[14306 + n * 31] = std::byte(size);
    for (unsigned j = 0; j < size; ++j)
      result[14307 + n * 31 + j] = std::byte(sample.name[j]);
  }
  for (const auto &sample : samples)
    for (unsigned frame = 0; frame < sample.frames; ++frame)
      for (unsigned channel = 0; channel < sample.channels; ++channel) {
        const double value =
            sample.amplitude * std::sin(frame * .13) * (channel ? -.5 : 1);
        const auto integer = std::int32_t(
            std::llround(value * (sample.bits == 16 ? 32768. : 8388608.)));
        for (unsigned byte = 0; byte < sample.bits / 8; ++byte)
          result.push_back(std::byte(unsigned(integer) >> (byte * 8)));
      }
  return result;
}
inline void writeGog(const std::filesystem::path &file,
                     std::span<const GogSample> samples,
                     std::span<const float> levels) {
  const auto bytes = gogBytes(samples, levels);
  cell::files::atomicWrite(file, bytes);
}
} // namespace cell_test
