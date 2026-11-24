// SPDX-License-Identifier: Apache-2.0
#include <algorithm>
#include <array>
#include <cell/pooled_file.hpp>
#include <limits>
#include <mutex>
#include <vector>
#if defined(_WIN32)
#define NOMINMAX
#include <windows.h>
#else
#include <cerrno>
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>
#endif
namespace cell::files {
namespace {
struct Identity {
  std::uint64_t device{}, file{}, seconds{}, fraction{}, bytes{};
  bool valid{};
  bool operator==(const Identity &) const = default;
};
#if defined(_WIN32)
using Handle = HANDLE;
const Handle invalidHandle = INVALID_HANDLE_VALUE;
void closeFile(Handle h) {
  if (h != invalidHandle)
    CloseHandle(h);
}
Handle openFile(const std::filesystem::path &p) {
  return CreateFileW(p.c_str(), GENERIC_READ,
                     FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                     nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
}
Identity inspect(Handle h) {
  BY_HANDLE_FILE_INFORMATION i{};
  if (h == invalidHandle || GetFileType(h) != FILE_TYPE_DISK ||
      !GetFileInformationByHandle(h, &i) ||
      (i.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY))
    return {};
  const auto size = (std::uint64_t(i.nFileSizeHigh) << 32) | i.nFileSizeLow;
  if (size > std::uint64_t(INT64_MAX))
    return {};
  return {i.dwVolumeSerialNumber,
          (std::uint64_t(i.nFileIndexHigh) << 32) | i.nFileIndexLow,
          i.ftLastWriteTime.dwHighDateTime,
          i.ftLastWriteTime.dwLowDateTime,
          size,
          true};
}
Identity inspect(const std::filesystem::path &p) {
  const auto h = openFile(p);
  const auto i = inspect(h);
  closeFile(h);
  return i;
}
#else
using Handle = int;
constexpr Handle invalidHandle = -1;
void closeFile(Handle h) {
  if (h >= 0)
    ::close(h);
}
Handle openFile(const std::filesystem::path &p) {
  return ::open(p.c_str(), O_RDONLY | O_CLOEXEC);
}
Identity identity(const struct stat &s) {
  if (!S_ISREG(s.st_mode) || s.st_size < 0)
    return {};
#if defined(__APPLE__)
  const auto modified = s.st_mtimespec;
#else
  const auto modified = s.st_mtim;
#endif
  return {std::uint64_t(s.st_dev),        std::uint64_t(s.st_ino),
          std::uint64_t(modified.tv_sec), std::uint64_t(modified.tv_nsec),
          std::uint64_t(s.st_size),       true};
}
Identity inspect(Handle h) {
  struct stat s{};
  return h >= 0 && ::fstat(h, &s) == 0 ? identity(s) : Identity{};
}
Identity inspect(const std::filesystem::path &p) {
  struct stat s{};
  return ::stat(p.c_str(), &s) == 0 ? identity(s) : Identity{};
}
#endif
} // namespace
struct PooledInputStream::Source {
  std::filesystem::path path;
  Identity identity;
};
struct ReadPool::Impl {
  struct Opened {
    Handle handle = invalidHandle;
    Identity identity;
    std::uint64_t use{};
  };
  std::vector<Opened> entries;
  explicit Impl(unsigned handles) : entries(handles) {}
  mutable std::mutex mutex;
  std::uint64_t clock{};
  Statistics counts;
  Handle acquire(const PooledInputStream::Source &source) {
    Opened *matched{}, *empty{}, *oldest = &entries[0];
    for (auto &entry : entries) {
      if (entry.handle != invalidHandle && entry.identity == source.identity) {
        matched = &entry;
        break;
      }
      if (entry.handle == invalidHandle && !empty)
        empty = &entry;
      if (entry.use < oldest->use)
        oldest = &entry;
    }
    auto *selected = matched ? matched : empty ? empty : oldest;
    if (selected->handle == invalidHandle ||
        selected->identity != source.identity) {
      if (selected->handle != invalidHandle) {
        closeFile(selected->handle);
        selected->handle = invalidHandle;
        --counts.opened;
      }
      const auto handle = openFile(source.path);
      if (handle == invalidHandle)
        return invalidHandle;
      if (inspect(handle) != source.identity) {
        closeFile(handle);
        return invalidHandle;
      }
      selected->handle = handle;
      selected->identity = source.identity;
      ++counts.opened;
      ++counts.opens;
      counts.peak = std::max(counts.peak, counts.opened);
    }
    selected->use = ++clock;
    return selected->handle;
  }
  ~Impl() {
    for (auto &entry : entries)
      closeFile(entry.handle);
  }
};
ReadPool::ReadPool(unsigned handles) : impl_(std::make_unique<Impl>(handles)) {}
ReadPool::~ReadPool() = default;
std::shared_ptr<ReadPool> ReadPool::create(unsigned handles) {
  if (handles < 1 || handles > 4096)
    throw std::invalid_argument(
        "Read-pool cache budget must be from 1 to 4096 handles");
  return std::shared_ptr<ReadPool>(new ReadPool(handles));
}
unsigned ReadPool::limit() const noexcept {
  return unsigned(impl_->entries.size());
}
std::shared_ptr<ReadPool> ReadPool::shared() {
  static std::mutex mutex;
  static std::weak_ptr<ReadPool> weak;
  std::lock_guard lock(mutex);
  auto result = weak.lock();
  if (!result) {
    result = std::shared_ptr<ReadPool>(new ReadPool);
    weak = result;
  }
  return result;
}
ReadPool::Statistics ReadPool::statistics() const {
  std::lock_guard lock(impl_->mutex);
  return impl_->counts;
}
bool ReadPool::admit(const PooledInputStream::Source &source) {
  std::lock_guard lock(impl_->mutex);
  return source.identity.valid && impl_->acquire(source) != invalidHandle;
}
int ReadPool::read(const PooledInputStream::Source &source, std::int64_t offset,
                   void *output, int maximum) {
  if (!source.identity.valid || !output || maximum <= 0 || offset < 0 ||
      std::uint64_t(offset) > source.identity.bytes)
    return 0;
  const auto wanted = unsigned(std::min<std::uint64_t>(
      unsigned(maximum), source.identity.bytes - std::uint64_t(offset)));
  if (!wanted)
    return 0;
  auto &p = *impl_;
  std::lock_guard lock(p.mutex);
  const auto handle = p.acquire(source);
  if (handle == invalidHandle)
    return 0;
  unsigned total{};
#if defined(_WIN32)
  LARGE_INTEGER where{};
  where.QuadPart = offset;
  if (!SetFilePointerEx(handle, where, nullptr, FILE_BEGIN))
    return 0;
  while (total < wanted) {
    DWORD count{};
    if (!ReadFile(handle, static_cast<std::byte *>(output) + total,
                  wanted - total, &count, nullptr) ||
        !count)
      break;
    total += count;
  }
#else
  while (total < wanted) {
    const auto count = ::pread(handle, static_cast<std::byte *>(output) + total,
                               wanted - total, off_t(offset) + off_t(total));
    if (count < 0 && errno == EINTR)
      continue;
    if (count <= 0)
      break;
    total += unsigned(count);
  }
#endif
  p.counts.bytes += total;
  return int(total);
}
PooledInputStream::PooledInputStream(const std::filesystem::path &p,
                                     std::shared_ptr<ReadPool> pool)
    : source_(std::make_unique<Source>(Source{p, inspect(p)})),
      pool_(std::move(pool)) {
  if (!pool_ || !source_->identity.valid || !pool_->admit(*source_))
    source_->identity.valid = false;
}
PooledInputStream::~PooledInputStream() = default;
bool PooledInputStream::isExhausted() const {
  return !openedOk() || getPosition() >= getTotalLength();
}
bool PooledInputStream::openedOk() const noexcept {
  return pool_ && source_->identity.valid;
}
int PooledInputStream::read(void *output, int maximum) {
  if (!openedOk())
    return 0;
  const auto count = pool_->read(*source_, position_, output, maximum);
  position_ += count;
  return count;
}
bool PooledInputStream::setPosition(std::int64_t at) {
  if (!openedOk() || at < 0 || std::uint64_t(at) > source_->identity.bytes)
    return false;
  position_ = at;
  return true;
}
std::int64_t PooledInputStream::getPosition() const { return position_; }
std::int64_t PooledInputStream::getTotalLength() const {
  return openedOk() ? std::int64_t(source_->identity.bytes) : 0;
}
} // namespace cell::files
