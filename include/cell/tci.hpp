// Original Cell Framework implementation; see LICENSE.
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

namespace cell::tci {
struct Limits {
  std::size_t records = 1000000, samples = 65536, labelBytes = 65536;
};
struct Record {
  std::uint32_t tag{}, bytes{};
  std::uint64_t offset{}; // Payload offset in the original container.
};
struct Sample {
  std::string name;
  std::uint32_t channels{}, scalarCount{}, bitCount{};
  std::uint64_t packedOffset{};
  std::uint64_t frames() const noexcept {
    return channels ? scalarCount / channels : 0;
  }
  // Observed v4 files store scalarCount - 1 values. WaveView supplies zero
  // for the absent final scalar, preserving the declared frame/channel shape.
  std::uint64_t encodedScalars() const noexcept {
    return scalarCount ? scalarCount - 1 : 0;
  }
};
struct Program {
  std::uint32_t version{}, rate{};
  std::vector<Sample> samples;
  // Uninterpreted mapping/articulation records remain addressable. Audio
  // decoding does not claim to emulate the instrument's detection or effects.
  std::vector<Record> records;
};
// Observed v4 embedded integer packing only. Preparation/cache-worker API:
// one shared file, an index per 201 scalar values, no extracted audio files.
class Source {
public:
  explicit Source(const std::filesystem::path &, Limits = {},
                  std::shared_ptr<cell::files::ReadPool> = {});
  const Program &program() const noexcept { return program_; }
  bool read(std::uint64_t offset, std::span<std::byte>);
  std::size_t decodeBlock(unsigned sample, std::size_t block,
                          std::span<std::byte> pcm24);

private:
  struct Block {
    std::uint64_t bit{};
    unsigned width{};
  };
  cell::files::PooledInputStream file_;
  std::uint64_t size_{};
  Program program_;
  std::vector<std::vector<Block>> blocks_;
  std::mutex mutex_;
};
Program inspect(const std::filesystem::path &, Limits = {});

// Seekable PCM24 WAVE bytes with the original interleaved sample order/rate.
// Each view caches one small decoded block. Reads/seeks stay off audio.
class WaveView {
public:
  WaveView(std::shared_ptr<Source>, unsigned sample);
  std::size_t read(std::span<std::byte>);
  bool seek(std::uint64_t position) noexcept;
  std::uint64_t position() const noexcept { return position_; }
  std::uint64_t length() const noexcept;

private:
  std::shared_ptr<Source> source_;
  unsigned sample_{};
  std::array<std::byte, 44> header_{};
  std::array<std::byte, 201 * 3> cache_{};
  std::size_t cachedBlock_ = std::size_t(-1), cachedBytes_{};
  std::uint64_t position_{}, pcmBytes_{};
};
} // namespace cell::tci
