// Original Cell Framework implementation; see LICENSE.
#pragma once
#include <cell/files.hpp>
#include <cell/pooled_file.hpp>
#include <fstream>
#include <functional>
#include <mutex>
#include <string>
#include <string_view>
#include <vector>

namespace cell::trg {
inline constexpr std::string_view fileExtension = ".trg";
inline constexpr unsigned maximumAssets = 65536;
inline constexpr std::size_t maximumMetadataBytes = 16u * 1024 * 1024;
using WorkCheck = std::function<void()>;
struct Entry {
  std::uint64_t offset{}, bytes{};
};
// Worker/control-thread API. JSON metadata and complete, unconverted WAVE
// assets share one file; individual views never extract an audio file.
class Source {
public:
  explicit Source(const std::filesystem::path &, const WorkCheck & = {},
                  std::shared_ptr<cell::files::ReadPool> = {});
  const std::string &metadata() const noexcept { return metadata_; }
  std::span<const Entry> entries() const noexcept { return entries_; }
  bool read(unsigned asset, std::uint64_t offset, std::span<std::byte>);

private:
  bool readAt(std::uint64_t, std::span<std::byte>);
  cell::files::PooledInputStream file_;
  std::uint64_t size_{};
  std::string metadata_;
  std::vector<Entry> entries_;
  std::mutex mutex_;
};
class WaveView {
public:
  WaveView(std::shared_ptr<Source>, unsigned asset);
  std::size_t read(std::span<std::byte>);
  bool seek(std::uint64_t) noexcept;
  std::uint64_t position() const noexcept { return position_; }
  std::uint64_t length() const noexcept;

private:
  std::shared_ptr<Source> source_;
  unsigned index_{};
  std::uint64_t position_{};
};
struct Asset {
  std::uint64_t bytes{};
  // Random-access read. Short reads are allowed; zero before EOF fails export.
  std::function<std::size_t(std::uint64_t, std::span<std::byte>)> read;
};
// Bounded streaming write with exclusive/explicit-replace atomic publication.
// A failed/cancelled write retains an existing destination. Callers must reject
// a destination that aliases an input asset before constructing the readers.
void write(const std::filesystem::path &, std::string_view metadata,
           std::span<const Asset>, bool replace = false,
           const WorkCheck & = {});
} // namespace cell::trg
