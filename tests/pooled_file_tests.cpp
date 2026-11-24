// SPDX-License-Identifier: Apache-2.0
#include "allocation_watch.hpp"
#include "sparse_fixture.hpp"
#include <atomic>
#include <cell/files.hpp>
#include <cell/pooled_file.hpp>
#include <cell/uuid.hpp>
#include <cstring>
#include <iostream>
#include <thread>
#if defined(__unix__)
#include <unistd.h>
#endif
namespace f = cell::files;
namespace {
void need(bool ok, const char *why) {
  if (!ok)
    throw std::runtime_error(why);
}
struct Folder {
  std::filesystem::path path =
      std::filesystem::temp_directory_path() /
      ("cell-file-pool-" + cell::Uuid().toString().toStdString());
  Folder() { std::filesystem::create_directories(path); }
  ~Folder() {
    std::error_code e;
    std::filesystem::remove_all(path, e);
  }
};
std::vector<std::byte> bytes(unsigned salt) {
  std::vector<std::byte> v(8193);
  for (unsigned n = 0; n < v.size(); ++n)
    v[n] = std::byte((salt * 37 + n * 19) & 255);
  return v;
}
void checkRead(f::PooledInputStream &stream, unsigned salt, std::uint64_t at) {
  const auto expected = bytes(salt);
  std::array<std::byte, 257> out{};
  need(stream.setPosition(std::int64_t(at)), "Pooled cursor seek failed");
  cell_test::allocations = cell_test::deletions = 0;
  cell_test::audio = true;
  const int count = stream.read(out.data(), int(out.size()));
  cell_test::audio = false;
  need(!cell_test::allocations && !cell_test::deletions,
       "Steady pooled worker read allocated/deleted C++ storage");
  need(count ==
               int(std::min<std::uint64_t>(out.size(), expected.size() - at)) &&
           std::equal(out.begin(), out.begin() + count,
                      expected.begin() + std::ptrdiff_t(at)),
       "Evicted pooled read changed source bytes");
  need(stream.getPosition() == std::int64_t(at + unsigned(count)),
       "Pooled logical cursor changed");
}
void cursorsAndLimits() {
  Folder folder;
  auto pool = f::ReadPool::shared();
  need(pool == f::ReadPool::shared(),
       "Sample decoders did not share their pool");
  {
    std::vector<std::unique_ptr<f::PooledInputStream>> streams;
    for (unsigned n = 0; n < 73; ++n) {
      const auto file = folder.path / ("sample-" + std::to_string(n));
      const auto data = bytes(n);
      cell::files::atomicWrite(file, data);
      streams.push_back(std::make_unique<f::PooledInputStream>(file, pool));
      need(streams.back()->openedOk() &&
               streams.back()->getTotalLength() == 8193,
           "Source metadata was not prepared");
    }
    for (unsigned round = 0; round < 4; ++round)
      for (unsigned n = 0; n < streams.size(); ++n)
        checkRead(*streams[n], n, (n * 31 + round * 111) % 8193);
    const auto stats = pool->statistics();
    need(stats.peak == f::ReadPool::capacity &&
             stats.opened <= f::ReadPool::capacity &&
             stats.opens > streams.size(),
         "File handles were not evicted/reopened within budget");
    checkRead(*streams[0], 0, 8193);
    need(!streams[0]->setPosition(-1) && !streams[0]->setPosition(8194),
         "Invalid pooled seek succeeded");
    std::array<std::byte, 8> buffer{};
    const auto position = streams[0]->getPosition();
    need(streams[0]->read(nullptr, 8) == 0 &&
             streams[0]->read(buffer.data(), -1) == 0 &&
             streams[0]->getPosition() == position,
         "Invalid read moved pooled cursor");
#if defined(__unix__)
    if (::geteuid() != 0) {
      const auto denied = folder.path / "denied";
      cell::files::atomicWrite(denied, bytes(9));
      std::filesystem::permissions(denied, std::filesystem::perms::none);
      f::PooledInputStream blocked(denied, pool);
      const bool rejected = !blocked.openedOk();
      std::filesystem::permissions(denied,
                                   std::filesystem::perms::owner_read |
                                       std::filesystem::perms::owner_write);
      need(rejected,
           "Unreadable source was admitted to decoder initialization");
    }
#endif
    f::PooledInputStream missing(folder.path / "missing", pool);
    need(!missing.openedOk() && missing.read(buffer.data(), 8) == 0 &&
             !missing.setPosition(0),
         "Missing pooled source was accepted");
    std::atomic<bool> okay{true};
    std::vector<std::thread> threads;
    for (unsigned worker = 0; worker < 8; ++worker)
      threads.emplace_back([&, worker] {
        try {
          f::PooledInputStream own(
              folder.path / ("sample-" + std::to_string(worker)), pool);
          for (unsigned n = 0; n < 100; ++n)
            checkRead(own, worker, (worker * 7 + n * 67) % 8193);
        } catch (...) {
          cell_test::audio = false;
          okay = false;
        }
      });
    for (auto &thread : threads)
      thread.join();
    need(okay, "Concurrent pooled streams crossed their logical cursors");
  }
  pool.reset();
  auto reopened = f::ReadPool::shared();
  need(reopened->statistics().opened == 0,
       "Last decoder did not release cached OS handles");
}
void isolated() {
  Folder folder;
  auto shared = f::ReadPool::shared(), pool = f::ReadPool::create(3);
  const auto before = shared->statistics();
  need(pool != shared && pool->limit() == 3,
       "Private file budget was not independent");
  for (unsigned n = 0; n < 24; ++n) {
    const auto file = folder.path / ("isolated-" + std::to_string(n));
    cell::files::atomicWrite(file, bytes(n));
    f::PooledInputStream input(file, pool);
    checkRead(input, n, 123);
  }
  const auto peak = pool->statistics();
  need(peak.peak == 3 && peak.opened == 3 &&
           shared->statistics().opens == before.opens,
       "Private work evicted/used the shared handle cache");
  const auto unicode = folder.path / std::filesystem::path(u8"鍵盤-é.raw");
  cell::files::atomicWrite(unicode, bytes(81));
  f::PooledInputStream input(unicode, pool);
  checkRead(input, 81, 4096);
  bool zero = false, large = false;
  try {
    (void)f::ReadPool::create(0);
  } catch (const std::invalid_argument &) {
    zero = true;
  }
  try {
    (void)f::ReadPool::create(4097);
  } catch (const std::invalid_argument &) {
    large = true;
  }
  need(zero && large, "Invalid handle cache budget was accepted");
}
void replacement() {
  Folder folder;
  auto pool = f::ReadPool::shared();
  const auto file = folder.path / "retained";
  cell::files::atomicWrite(file, bytes(1));
  f::PooledInputStream old(file, pool);
  checkRead(old, 1, 0);
  cell::files::atomicWrite(file, bytes(2), true);
  checkRead(old, 1, 0);
  for (unsigned n = 0; n < f::ReadPool::capacity + 1; ++n) {
    const auto p = folder.path / ("evict-" + std::to_string(n));
    cell::files::atomicWrite(p, bytes(n + 3));
    f::PooledInputStream stream(p, pool);
    checkRead(stream, n + 3, 0);
  }
  std::array<std::byte, 16> data{};
  need(old.setPosition(0) && old.read(data.data(), 16) == 0,
       "An old prepared stream read a replacement file after eviction");
  f::PooledInputStream next(file, pool);
  checkRead(next, 2, 0);
}
void sparse() {
  Folder folder;
  const auto file = folder.path / "long";
  constexpr std::int64_t at = (INT64_C(1) << 32) + 17;
  {
    const cell::File fixture(file);
    cell::FileOutputStream out(fixture, cellTest::prepareSparseFixture(fixture));
    need(out.openedOk() && out.setPosition(at) && out.write("hello", 5),
         "Sparse stream fixture failed");
    out.flush();
    need(out.getStatus().wasOk(), "Sparse stream fixture flush failed");
  }
  f::PooledInputStream stream(file, f::ReadPool::shared());
  std::array<char, 5> text{};
  need(stream.getTotalLength() == at + 5 && stream.setPosition(at) &&
           stream.read(text.data(), 5) == 5 &&
           std::memcmp(text.data(), "hello", 5) == 0,
       "Pooled file offset narrowed to 32 bits");
}
} // namespace
int main() {
  try {
    cursorsAndLimits();
    isolated();
    replacement();
    sparse();
    std::cout << "PASS sample file pool: bounded handles, retained decoder "
                 "cursors, repeated eviction/reopen, concurrent shared "
                 "sources, replacement rejection, 64-bit sparse offsets, "
                 "worker allocation guards and last-owner cleanup\n";
    return 0;
  } catch (const std::exception &e) {
    cell_test::audio = false;
    std::cerr << e.what() << '\n';
    return 1;
  }
}
