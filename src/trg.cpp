// Original Cell Framework implementation; see LICENSE.
#include <algorithm>
#include <array>
#include <cell/trg.hpp>
#include <cell/json.hpp>
#include <cstring>
#include <limits>

namespace cell::trg {
namespace {
constexpr std::size_t headerBytes = 40;
constexpr std::string_view magic = "CELLTRG1";
void need(bool ok, const char *why) {
  if (!ok)
    throw std::runtime_error(why);
}
std::uint64_t add(std::uint64_t a, std::uint64_t b) {
  need(a <= std::uint64_t(INT64_MAX) && b <= std::uint64_t(INT64_MAX) - a,
       "TRG file size exceeds 64-bit addressing");
  return a + b;
}
std::uint64_t aligned(std::uint64_t n) { return add(n, 7) & ~UINT64_C(7); }
std::uint64_t le(std::span<const std::byte> b, unsigned at, unsigned count) {
  std::uint64_t n{};
  for (unsigned i = 0; i < count; ++i)
    n |= std::uint64_t(std::to_integer<unsigned>(b[at + i])) << (i * 8);
  return n;
}
void put(std::span<std::byte> b, unsigned at, std::uint64_t n, unsigned count) {
  for (unsigned i = 0; i < count; ++i)
    b[at + i] = std::byte((n >> (i * 8)) & 255);
}
void metadataValid(std::string_view s) {
  need(!s.empty() && s.size() <= maximumMetadataBytes,
       "TRG metadata exceeds the supported size");
  json::Limits limits;
  limits.inputBytes = limits.textBytes = maximumMetadataBytes;
  limits.nodes = 1048576;
  need(json::read(s, limits).isObject(), "TRG metadata must be a JSON object");
}
void waveValid(std::span<const std::byte> s) {
  const std::string_view tag(reinterpret_cast<const char *>(s.data()), 4);
  need((tag == "RIFF" || tag == "RIFX" || tag == "RF64" || tag == "BW64") &&
           std::memcmp(s.data() + 8, "WAVE", 4) == 0,
       "TRG asset is not a WAVE source");
}
void readAsset(const Asset &a, std::uint64_t at, std::span<std::byte> into) {
  while (!into.empty()) {
    const auto count = a.read(at, into);
    need(count && count <= into.size(), "TRG source ended early");
    at += count;
    into = into.subspan(count);
  }
}
} // namespace
Source::Source(const std::filesystem::path &path, const WorkCheck &check,
               std::shared_ptr<cell::files::ReadPool> pool)
    : file_(path, pool ? std::move(pool) : cell::files::ReadPool::shared()) {
  if (check)
    check();
  need(file_.openedOk(), "Cannot open TRG library");
  const auto end = file_.getTotalLength();
  need(end >= std::streamoff(headerBytes), "Truncated TRG library");
  size_ = std::uint64_t(end);
  std::array<std::byte, headerBytes> h{};
  need(readAt(0, h), "Cannot read TRG header");
  need(std::memcmp(h.data(), magic.data(), magic.size()) == 0 &&
           le(h, 8, 4) == 1 && le(h, 12, 4) == 0,
       "Unsupported TRG library version or flags");
  const auto metadataBytes = le(h, 16, 4), count = le(h, 20, 4);
  need(metadataBytes && metadataBytes <= maximumMetadataBytes &&
           count <= maximumAssets && le(h, 32, 8) == size_,
       "Invalid TRG dimensions or truncated library");
  auto next = aligned(add(add(headerBytes, metadataBytes), count * 16));
  need(le(h, 24, 8) == next && next <= size_, "Invalid TRG asset table");
  metadata_.resize(std::size_t(metadataBytes));
  need(readAt(headerBytes, std::as_writable_bytes(
                               std::span(metadata_.data(), metadata_.size()))),
       "Cannot read TRG metadata");
  metadataValid(metadata_);
  std::vector<std::byte> table(std::size_t(count) * 16);
  need(readAt(headerBytes + metadataBytes, table),
       "Cannot read TRG asset table");
  entries_.reserve(std::size_t(count));
  for (unsigned n = 0; n < count; ++n) {
    if (check)
      check();
    Entry e{le(table, n * 16, 8), le(table, n * 16 + 8, 8)};
    need(e.offset == next && e.bytes >= 12 && e.bytes <= size_ - next,
         "Invalid TRG asset range or overlapping assets");
    std::array<std::byte, 12> header{};
    need(readAt(e.offset, header), "Cannot read TRG WAVE header");
    waveValid(header);
    entries_.push_back(e);
    next = add(e.offset, e.bytes);
    if (n + 1 < count) {
      next = aligned(next);
      need(next <= size_, "Truncated TRG alignment");
    }
  }
  need(next == size_, "Unindexed bytes at end of TRG library");
  if (check)
    check();
}
bool Source::readAt(std::uint64_t at, std::span<std::byte> out) {
  if (at > size_ || out.size() > size_ - at ||
      out.size() > std::size_t(std::numeric_limits<std::streamsize>::max()))
    return false;
  if (out.empty())
    return true;
  return file_.setPosition(std::int64_t(at)) && file_.readFully(out);
}
bool Source::read(unsigned index, std::uint64_t at, std::span<std::byte> out) {
  if (index >= entries_.size())
    return false;
  const auto &e = entries_[index];
  if (at > e.bytes || out.size() > e.bytes - at)
    return false;
  std::lock_guard lock(mutex_);
  return readAt(e.offset + at, out);
}
WaveView::WaveView(std::shared_ptr<Source> s, unsigned n)
    : source_(std::move(s)), index_(n) {
  need(source_ && n < source_->entries().size(),
       "TRG sample index is out of range");
}
std::size_t WaveView::read(std::span<std::byte> into) {
  const auto size =
      std::size_t(std::min<std::uint64_t>(into.size(), length() - position_));
  if (!source_->read(index_, position_, into.first(size)))
    return 0;
  position_ += size;
  return size;
}
bool WaveView::seek(std::uint64_t at) noexcept {
  if (at > length())
    return false;
  position_ = at;
  return true;
}
std::uint64_t WaveView::length() const noexcept {
  return source_->entries()[index_].bytes;
}
void write(const std::filesystem::path &path, std::string_view metadata,
           std::span<const Asset> assets, bool replace,
           const WorkCheck &check) {
  if (check)
    check();
  metadataValid(metadata);
  need(assets.size() <= maximumAssets, "Too many TRG assets");
  std::vector<std::byte> prefix(
      std::size_t(aligned(headerBytes + metadata.size() + assets.size() * 16)));
  std::memcpy(prefix.data(), magic.data(), 8);
  put(prefix, 8, 1, 4);
  put(prefix, 16, metadata.size(), 4);
  put(prefix, 20, assets.size(), 4);
  put(prefix, 24, prefix.size(), 8);
  std::memcpy(prefix.data() + headerBytes, metadata.data(), metadata.size());
  std::uint64_t end = prefix.size();
  for (unsigned n = 0; n < assets.size(); ++n) {
    if (check)
      check();
    const auto &a = assets[n];
    need(a.bytes >= 12 && bool(a.read), "Invalid TRG source asset");
    std::array<std::byte, 12> h{};
    readAsset(a, 0, h);
    waveValid(h);
    const auto at = unsigned(headerBytes + metadata.size() + n * 16);
    put(prefix, at, end, 8);
    put(prefix, at + 8, a.bytes, 8);
    end = add(end, a.bytes);
    if (n + 1 < assets.size())
      end = aligned(end);
  }
  put(prefix, 32, end, 8);
  files::AtomicWriter output(path, replace);
  output.write(prefix);
  std::array<std::byte, 65536> buffer{};
  std::array<std::byte, 8> zero{};
  for (unsigned n = 0; n < assets.size(); ++n) {
    const auto &a = assets[n];
    std::uint64_t at{};
    while (at < a.bytes) {
      if (check)
        check();
      const auto size =
          std::size_t(std::min<std::uint64_t>(a.bytes - at, buffer.size()));
      auto into = std::span(buffer).first(size);
      readAsset(a, at, into);
      output.write(into);
      at += size;
    }
    if (n + 1 < assets.size())
      output.write(
          std::span(zero).first(std::size_t(aligned(a.bytes) - a.bytes)));
  }
  if (check)
    check();
  output.commit();
}
} // namespace cell::trg
