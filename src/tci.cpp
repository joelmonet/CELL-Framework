// Original Cell Framework implementation; see LICENSE.
#include <algorithm>
#include <cell/tci.hpp>
#include <cstring>
#include <limits>
#include <stdexcept>

namespace cell::tci {
namespace {
void require(bool condition, const char *message) {
  if (!condition)
    throw std::invalid_argument(message);
}
std::uint32_t u32(std::span<const std::byte> bytes) {
  std::uint32_t value{};
  for (unsigned n = 0; n < 4; ++n)
    value |= std::uint32_t(std::to_integer<unsigned>(bytes[n])) << (n * 8);
  return value;
}
// Indexing reads each compressed region through one bounded buffer, rather
// than seeking the filesystem for every small block header.
class Buffered {
  cell::files::PooledInputStream &file_;
  std::uint64_t size_{}, start_{};
  std::size_t count_{};
  std::array<std::byte, 65536> bytes_{};

public:
  Buffered(cell::files::PooledInputStream &file, std::uint64_t size)
      : file_(file), size_(size) {}
  unsigned byte(std::uint64_t position) {
    require(position < size_, "Truncated .tci input");
    if (position < start_ || position - start_ >= count_) {
      start_ = position;
      count_ =
          std::size_t(std::min<std::uint64_t>(bytes_.size(), size_ - start_));
      if (!file_.setPosition(std::int64_t(start_)) ||
          !file_.readFully(std::span(bytes_).first(count_)))
        throw std::runtime_error("Could not read .tci input");
    }
    return std::to_integer<unsigned>(bytes_[std::size_t(position - start_)]);
  }
  std::uint32_t integer(std::uint64_t at) {
    std::array<std::byte, 4> b{};
    for (unsigned n = 0; n < 4; ++n)
      b[n] = std::byte(byte(at + n));
    return u32(b);
  }
  unsigned width(std::uint64_t at, unsigned shift) {
    const auto a = byte(at);
    return shift ? ((a << 8) | byte(at + 1)) >> (8 - shift) & 255 : a;
  }
};
void put(std::span<std::byte> bytes, std::size_t at, std::uint32_t value,
         unsigned count) {
  for (unsigned n = 0; n < count; ++n)
    bytes[at + n] = std::byte((value >> (n * 8)) & 255);
}
// Labels are display data, never asset paths. Preserve valid UTF-8; older
// single-byte labels are represented as Latin-1 if their UTF-8 is malformed.
std::string label(std::string raw) {
  bool valid = true;
  for (std::size_t i = 0; i < raw.size();) {
    auto c = static_cast<unsigned char>(raw[i++]);
    if (c < 128)
      continue;
    unsigned count = c >= 0xc2 && c <= 0xdf   ? 1
                     : c >= 0xe0 && c <= 0xef ? 2
                     : c >= 0xf0 && c <= 0xf4 ? 3
                                              : 99;
    unsigned value = c & (count == 1 ? 31 : count == 2 ? 15 : 7);
    if (count == 99 || count > raw.size() - i) {
      valid = false;
      break;
    }
    for (unsigned n = 0; n < count; ++n) {
      const auto next = static_cast<unsigned char>(raw[i++]);
      if ((next & 0xc0) != 0x80) {
        valid = false;
        break;
      }
      value = (value << 6) | (next & 63);
    }
    if (!valid || value > 0x10ffff || (value >= 0xd800 && value <= 0xdfff) ||
        (count == 1 && value < 128) || (count == 2 && value < 2048) ||
        (count == 3 && value < 65536)) {
      valid = false;
      break;
    }
  }
  if (valid)
    return raw;
  std::string text;
  for (unsigned char c : raw) {
    if (c < 128)
      text += char(c);
    else {
      text += char(0xc0 | (c >> 6));
      text += char(0x80 | (c & 63));
    }
  }
  return text;
}
} // namespace

Source::Source(const std::filesystem::path &path, Limits limits,
               std::shared_ptr<cell::files::ReadPool> pool)
    : file_(path, pool ? std::move(pool) : cell::files::ReadPool::shared()) {
  if (!file_.openedOk())
    throw std::runtime_error("Could not open .tci file");
  const auto length = file_.getTotalLength();
  require(length >= 64, "Invalid .tci size");
  size_ = std::uint64_t(length);
  Buffered input(file_, size_);
  constexpr char magic[] = "TRIGGER COMPRESSED INSTRUMENT";
  for (unsigned n = 0; n < 64; ++n)
    require(input.byte(n) == (n < sizeof(magic) - 1 ? unsigned(magic[n]) : 0),
            "Unsupported .tci header");
  bool version = false, rate = false, count = false, ended = false;
  std::uint32_t declared{};
  for (std::uint64_t at = 64; at < size_;) {
    require(size_ - at >= 8 && program_.records.size() < limits.records,
            "Invalid .tci record count/header");
    const auto tag = input.integer(at), bytes = input.integer(at + 4);
    at += 8;
    require(bytes <= size_ - at, "Truncated .tci record");
    program_.records.push_back({tag, bytes, at});
    if (tag == 1) {
      require(!version && bytes == 4, "Invalid .tci version record");
      version = true;
      program_.version = input.integer(at);
      require(program_.version == 4,
              "Unsupported .tci version; expected observed v4");
    } else if (tag == 20) {
      require(!rate && bytes == 4, "Invalid .tci rate record");
      rate = true;
      program_.rate = input.integer(at);
      require(program_.rate >= 8000 && program_.rate <= 384000,
              "Invalid .tci rate");
    } else if (tag == 4) {
      require(!count && bytes == 4, "Invalid .tci sample-count record");
      count = true;
      declared = input.integer(at);
      require(declared && declared <= limits.samples,
              "TCI sample count exceeds admission budget");
    } else if (tag == 5) {
      require(version && count && program_.samples.size() < declared &&
                  bytes >= 10,
              "Invalid .tci embedded sample record");
      Sample sample;
      sample.channels = input.byte(at);
      sample.bitCount = input.integer(at + 1);
      sample.scalarCount = input.integer(at + 5);
      sample.packedOffset = at + 9;
      require((sample.channels == 1 || sample.channels == 2) &&
                  sample.scalarCount >= sample.channels * 2 &&
                  sample.scalarCount % sample.channels == 0,
              "Invalid .tci sample dimensions");
      const auto packed = std::uint64_t(sample.bitCount) / 8 + 1;
      require(packed <= bytes - 9 && bytes - 9 - packed <= limits.labelBytes,
              "Invalid .tci packing/label extent");
      const auto scalars = sample.encodedScalars();
      const auto blockCount = (scalars + 200) / 201;
      require(scalars + blockCount * 8 <= sample.bitCount,
              "TCI dimensions exceed encoded bit budget");
      std::vector<Block> blocks;
      blocks.reserve(std::size_t(blockCount));
      std::uint64_t bit{};
      for (std::uint64_t done = 0; done < scalars;) {
        require(bit <= sample.bitCount && sample.bitCount - bit >= 8,
                "Truncated .tci block width");
        const auto width =
            input.width(sample.packedOffset + bit / 8, unsigned(bit % 8));
        bit += 8;
        require(width >= 1 && width <= 25,
                "Unsupported .tci integer packing width");
        const auto amount = std::min<std::uint64_t>(201, scalars - done);
        require(amount * width <= sample.bitCount - bit,
                "Truncated .tci packed block");
        blocks.push_back({bit, width});
        bit += amount * width;
        done += amount;
      }
      require(bit == sample.bitCount, "Unsupported .tci packing dimensions");
      std::string name;
      for (std::uint64_t n = packed + 9; n < bytes; ++n) {
        const auto c = input.byte(at + n);
        require(c != 0, "Invalid .tci sample label");
        name += char(c);
      }
      sample.name = label(std::move(name));
      program_.samples.push_back(std::move(sample));
      blocks_.push_back(std::move(blocks));
    } else if (tag == 3) {
      require(bytes == 0 && at == size_, "Invalid .tci end marker");
      ended = true;
    }
    at += bytes;
  }
  require(version && rate && count && ended &&
              program_.samples.size() == declared,
          "Incomplete .tci embedded instrument");
}

bool Source::read(std::uint64_t offset, std::span<std::byte> bytes) {
  if (offset > size_ || bytes.size() > size_ - offset ||
      bytes.size() > std::size_t(std::numeric_limits<std::streamsize>::max()))
    return false;
  if (bytes.empty())
    return true;
  std::lock_guard lock(mutex_);
  return file_.setPosition(std::int64_t(offset)) && file_.readFully(bytes);
}

std::size_t Source::decodeBlock(unsigned sampleIndex, std::size_t blockIndex,
                                std::span<std::byte> pcm) {
  require(sampleIndex < program_.samples.size(), "Invalid .tci sample index");
  const auto &sample = program_.samples[sampleIndex];
  const auto &blocks = blocks_[sampleIndex];
  require(blockIndex < blocks.size(), "Invalid .tci block index");
  const auto &block = blocks[blockIndex];
  const auto count = std::size_t(std::min<std::uint64_t>(
      201, sample.encodedScalars() - blockIndex * 201ull));
  require(pcm.size() >= count * 3,
          "TCI decoded-block destination is too small");
  std::array<std::byte, 630> encoded{};
  const auto skip = unsigned(block.bit % 8);
  const auto bytes = (skip + count * block.width + 7) / 8;
  require(bytes <= encoded.size(), "Invalid .tci block size");
  if (!read(sample.packedOffset + block.bit / 8,
            std::span(encoded).first(bytes)))
    throw std::runtime_error("Could not read .tci audio block");
  std::size_t bit = skip;
  for (std::size_t n = 0; n < count; ++n) {
    std::uint32_t raw{};
    const auto byteCount = (unsigned(bit % 8) + block.width + 7) / 8;
    for (unsigned j = 0; j < byteCount; ++j)
      raw = (raw << 8) | std::to_integer<unsigned>(encoded[bit / 8 + j]);
    raw = (raw >> (byteCount * 8 - unsigned(bit % 8) - block.width)) &
          ((std::uint32_t(1) << block.width) - 1);
    bit += block.width;
    const auto sign = std::uint32_t(1) << (block.width - 1);
    const auto value =
        raw & sign ? -std::int32_t(raw & (sign - 1)) : std::int32_t(raw);
    require(value >= -8388608 && value <= 8388607,
            "TCI value exceeds observed PCM24 range");
    put(pcm, n * 3, std::uint32_t(value), 3);
  }
  return count * 3;
}

Program inspect(const std::filesystem::path &path, Limits limits) {
  Source source(path, limits);
  return source.program();
}

WaveView::WaveView(std::shared_ptr<Source> source, unsigned sample)
    : source_(std::move(source)), sample_(sample) {
  require(source_ && sample < source_->program().samples.size(),
          "Invalid .tci WAVE sample");
  const auto &info = source_->program().samples[sample];
  pcmBytes_ = std::uint64_t(info.scalarCount) * 3;
  require(pcmBytes_ + (pcmBytes_ & 1) <= UINT32_MAX - 36,
          "TCI sample exceeds RIFF WAVE view capacity");
  std::memcpy(header_.data(), "RIFF", 4);
  put(header_, 4, std::uint32_t(length() - 8), 4);
  std::memcpy(header_.data() + 8, "WAVEfmt ", 8);
  put(header_, 16, 16, 4);
  put(header_, 20, 1, 2);
  put(header_, 22, info.channels, 2);
  put(header_, 24, source_->program().rate, 4);
  put(header_, 28, source_->program().rate * info.channels * 3, 4);
  put(header_, 32, info.channels * 3, 2);
  put(header_, 34, 24, 2);
  std::memcpy(header_.data() + 36, "data", 4);
  put(header_, 40, std::uint32_t(pcmBytes_), 4);
}
std::uint64_t WaveView::length() const noexcept {
  return 44 + pcmBytes_ + (pcmBytes_ & 1);
}
bool WaveView::seek(std::uint64_t position) noexcept {
  if (position > length())
    return false;
  position_ = position;
  return true;
}
std::size_t WaveView::read(std::span<std::byte> output) {
  std::size_t written{};
  while (written < output.size() && position_ < length()) {
    if (position_ < header_.size()) {
      const auto count = std::min<std::size_t>(
          output.size() - written, header_.size() - std::size_t(position_));
      std::copy_n(header_.begin() + std::ptrdiff_t(position_), count,
                  output.begin() + std::ptrdiff_t(written));
      position_ += count;
      written += count;
    } else {
      const auto relative = position_ - header_.size();
      const auto encoded =
          source_->program().samples[sample_].encodedScalars() * 3;
      if (relative >= encoded) {
        const auto count = std::size_t(std::min<std::uint64_t>(
            output.size() - written, length() - position_));
        std::fill_n(output.begin() + std::ptrdiff_t(written), count,
                    std::byte{});
        position_ += count;
        written += count;
      } else {
        const auto block = std::size_t(relative / cache_.size());
        if (cachedBlock_ != block) {
          cachedBlock_ = std::size_t(-1);
          cachedBytes_ = source_->decodeBlock(sample_, block, cache_);
          cachedBlock_ = block;
        }
        const auto offset = std::size_t(relative % cache_.size());
        require(offset < cachedBytes_, "Invalid .tci decoded block extent");
        const auto count =
            std::min(output.size() - written, cachedBytes_ - offset);
        std::copy_n(cache_.begin() + std::ptrdiff_t(offset), count,
                    output.begin() + std::ptrdiff_t(written));
        position_ += count;
        written += count;
      }
    }
  }
  return written;
}
} // namespace cell::tci
