// SPDX-License-Identifier: Apache-2.0
#include <algorithm>
#include <bit>
#include <cell/gog.hpp>
#include <cmath>
#include <stdexcept>

namespace cell::gog {
namespace {
void require(bool ok, const char *why) {
  if (!ok)
    throw std::invalid_argument(why);
}
unsigned byte(std::span<const std::byte> b, std::size_t at) {
  return std::to_integer<unsigned>(b[at]);
}
unsigned u16(std::span<const std::byte> b, std::size_t at) {
  return byte(b, at) | (byte(b, at + 1) << 8);
}
unsigned u32(std::span<const std::byte> b, std::size_t at) {
  return u16(b, at) | (u16(b, at + 2) << 16);
}
// Old filenames use single-byte text. ASCII is unchanged; high bytes are
// preserved as Unicode Latin-1 display text, never interpreted as asset paths.
std::string name(std::span<const std::byte> b, std::size_t at) {
  const unsigned count = byte(b, at);
  require(count <= 30, "Invalid .gog sample-name length");
  std::string result;
  for (unsigned n = 0; n < count; ++n) {
    const auto c = byte(b, at + 1 + n);
    require(c != 0, "Invalid .gog sample name");
    if (c < 128)
      result += char(c);
    else {
      result += char(0xc0 | (c >> 6));
      result += char(0x80 | (c & 63));
    }
  }
  return result;
}
} // namespace
Program parse(std::span<const std::byte> b, std::uint64_t size) {
  require(b.size() == headerBytes && size >= headerBytes &&
              size <= 2ull * 1024 * 1024 * 1024,
          "Invalid or oversized .gog container");
  constexpr char magic[] = "Gog_v3.5";
  for (unsigned n = 0; n < 8; ++n)
    require(byte(b, n) == unsigned(magic[n]),
            "This .gog layout is not supported; expected v3.5 PCM");
  const unsigned count = u16(b, 366), groups = u16(b, 368);
  require(count && count <= maximumSamples && groups && groups <= maximumGroups,
          "Invalid .gog sample/group count");
  Program result;
  result.groups = groups;
  result.samples.reserve(count);
  for (unsigned group = 0; group < groups; ++group) {
    const auto level = std::bit_cast<float>(u32(b, 370 + group * 4));
    require(std::isfinite(level) && level >= 0 && level <= 1,
            "Invalid .gog dynamic-group level");
    result.levels[group] = level;
  }
  std::uint64_t offset = headerBytes;
  for (unsigned n = 0; n < count; ++n) {
    const unsigned scalars = u32(b, 26210 + n * 4),
                   rate = u32(b, 29666 + n * 4), stereo = byte(b, 33506 + n),
                   bits = byte(b, 33890 + n), group = u16(b, 28898 + n * 2),
                   enabled = byte(b, 28514 + n);
    require(stereo <= 1 && enabled <= 1 && (bits == 16 || bits == 24),
            "Unsupported .gog PCM encoding or sample flags");
    const unsigned channels = stereo + 1;
    require(scalars && scalars % channels == 0 && scalars / channels >= 2 &&
                scalars / channels <= 100000000 && rate >= 8000 &&
                rate <= 384000 && group >= 1 && group <= groups,
            "Invalid .gog sample dimensions/group");
    const auto bytes = std::uint64_t(scalars) * (bits / 8);
    require(offset <= size && bytes <= size - offset,
            "Truncated .gog sample data");
    result.samples.push_back({name(b, 14306 + n * 31), offset, bytes,
                              scalars / channels, rate, channels, bits,
                              group - 1, enabled != 0});
    offset += bytes;
  }
  // An extension with extra microphone/compressed payloads must not silently
  // be interpreted as the known primary PCM layout.
  require(offset == size, "Unsupported .gog extension/payload layout");
  return result;
}
Source::Source(const std::filesystem::path &path,
               std::shared_ptr<cell::files::ReadPool> pool)
    : file_(path, pool ? std::move(pool) : cell::files::ReadPool::shared()) {
  if (!file_.openedOk())
    throw std::runtime_error("Could not open .gog file");
  const auto length = file_.getTotalLength();
  if (length < 0)
    throw std::runtime_error("Could not size .gog file");
  size_ = std::uint64_t(length);
  require(size_ >= headerBytes && size_ <= 2ull * 1024 * 1024 * 1024,
          "Invalid or oversized .gog file");
  std::array<std::byte, headerBytes> header{};
  if (!file_.setPosition(0) || !file_.readFully(header))
    throw std::runtime_error("Could not read .gog header");
  program_ = parse(header, size_);
}
bool Source::read(std::uint64_t offset, std::span<std::byte> out) {
  if (offset > size_ || out.size() > size_ - offset)
    return false;
  std::lock_guard lock(mutex_);
  return file_.setPosition(std::int64_t(offset)) && file_.readFully(out);
}
Program inspect(const std::filesystem::path &path) {
  return Source(path).program();
}

WaveView::WaveView(std::shared_ptr<Source> source, unsigned index)
    : source_(std::move(source)) {
  if (!source_ || index >= source_->program().samples.size())
    throw std::invalid_argument("Invalid .gog sample index");
  sample_ = source_->program().samples[index];
  const auto put16 = [&](unsigned at, unsigned value) {
    header_[at] = std::byte(value);
    header_[at + 1] = std::byte(value >> 8);
  };
  const auto put32 = [&](unsigned at, unsigned value) {
    put16(at, value);
    put16(at + 2, value >> 16);
  };
  put32(0, 0x46464952);
  put32(4, unsigned(sample_.bytes) + (unsigned(sample_.bytes) & 1u) + 36);
  put32(8, 0x45564157);
  put32(12, 0x20746d66);
  put32(16, 16);
  put16(20, 1);
  put16(22, sample_.channels);
  put32(24, sample_.rate);
  put32(28, sample_.rate * sample_.channels * (sample_.bits / 8));
  put16(32, sample_.channels * (sample_.bits / 8));
  put16(34, sample_.bits);
  put32(36, 0x61746164);
  put32(40, unsigned(sample_.bytes));
}

std::size_t WaveView::read(std::span<std::byte> out) {
  const auto count =
      std::size_t(std::min<std::uint64_t>(out.size(), length() - position_));
  std::size_t copied = 0;
  if (position_ < header_.size()) {
    const auto part = std::min(count, header_.size() - std::size_t(position_));
    std::copy_n(header_.data() + position_, part, out.data());
    position_ += part;
    copied += part;
  }
  if (copied < count && position_ - header_.size() < sample_.bytes) {
    const auto part = std::size_t(std::min<std::uint64_t>(
        count - copied, sample_.bytes - (position_ - header_.size())));
    if (!source_->read(sample_.offset + position_ - header_.size(),
                       out.subspan(copied, part)))
      return copied;
    position_ += part;
    copied += part;
  }
  if (copied < count) {
    out[copied++] = std::byte{};
    ++position_;
  }
  return copied;
}

bool WaveView::seek(std::uint64_t at) noexcept {
  if (at > length())
    return false;
  position_ = at;
  return true;
}

std::uint64_t WaveView::length() const noexcept {
  return 44 + sample_.bytes + (sample_.bytes & 1);
}
} // namespace cell::gog
