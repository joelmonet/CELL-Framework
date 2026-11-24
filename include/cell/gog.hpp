// SPDX-License-Identifier: Apache-2.0
#pragma once
#include <array>
#include <cell/pooled_file.hpp>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <memory>
#include <mutex>
#include <span>
#include <string>
#include <vector>

namespace cell::gog {
inline constexpr unsigned maximumSamples = 384, maximumGroups = 16,
                          headerBytes = 42844;
struct Sample {
  std::string name;
  std::uint64_t offset{}, bytes{};
  unsigned frames{}, rate{}, channels{}, bits{}, group{};
  bool enabled = true;
};
struct Program {
  std::vector<Sample> samples;
  std::array<float, maximumGroups> levels{};
  unsigned groups{};
};
// The v3.5 PCM layout is read independently. No decoder or vendor code is
// required. Other layouts are rejected before any playable state is changed.
Program parse(std::span<const std::byte> header, std::uint64_t fileBytes);
Program inspect(const std::filesystem::path &);
// One opened source is shared by its sample views. Only preparation/cache
// workers read it, so a large multisample does not require hundreds of handles.
class Source {
public:
  explicit Source(const std::filesystem::path &,
                  std::shared_ptr<cell::files::ReadPool> = {});
  const Program &program() const noexcept { return program_; }
  bool read(std::uint64_t offset, std::span<std::byte>);

private:
  cell::files::PooledInputStream file_;
  std::uint64_t size_{};
  Program program_;
  std::mutex mutex_;
};

// A seekable PCM WAVE view of one embedded sample. Use on preparation/cache
// workers, never in a real-time callback. No extraction or audio conversion is
// performed; independent format decoders can consume the returned bytes.
class WaveView {
public:
  WaveView(std::shared_ptr<Source>, unsigned sampleIndex);
  std::size_t read(std::span<std::byte>);
  bool seek(std::uint64_t position) noexcept;
  std::uint64_t position() const noexcept { return position_; }
  std::uint64_t length() const noexcept;

private:
  std::shared_ptr<Source> source_;
  Sample sample_;
  std::array<std::byte, 44> header_{};
  std::uint64_t position_{};
};
} // namespace cell::gog
