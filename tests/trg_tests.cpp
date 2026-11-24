// Original Cell Framework implementation; see LICENSE.
#include <array>
#include <atomic>
#include <cell/trg.hpp>
#include <cell/uuid.hpp>
#include <chrono>
#include <cstring>
#include <iostream>
#include <thread>
using namespace cell;
namespace {
void need(bool ok, const char *why) {
  if (!ok)
    throw std::runtime_error(why);
}
template <class F> void rejects(F f) {
  bool failed{};
  try {
    f();
  } catch (const std::exception &) {
    failed = true;
  }
  need(failed, "Invalid TRG operation succeeded");
}
struct Folder {
  std::filesystem::path path = std::filesystem::temp_directory_path() /
                               ("cell-trg-" + Uuid().toString().toStdString());
  Folder() { std::filesystem::create_directories(path); }
  ~Folder() {
    std::error_code e;
    std::filesystem::remove_all(path, e);
  }
};
void put(std::span<std::byte> b, unsigned at, std::uint64_t n, unsigned count) {
  for (unsigned i = 0; i < count; ++i)
    b[at + i] = std::byte((n >> (8 * i)) & 255);
}
std::uint64_t le(std::span<const std::byte> b, unsigned at, unsigned count) {
  std::uint64_t r{};
  for (unsigned i = 0; i < count; ++i)
    r |= std::uint64_t(std::to_integer<unsigned>(b[at + i])) << (8 * i);
  return r;
}
std::vector<std::byte> wave(unsigned salt, unsigned count) {
  std::vector<std::byte> b(44 + count);
  std::memcpy(b.data(), "RIFF", 4);
  put(b, 4, b.size() - 8, 4);
  std::memcpy(b.data() + 8, "WAVEfmt ", 8);
  put(b, 16, 16, 4);
  put(b, 20, 1, 2);
  put(b, 22, 1, 2);
  put(b, 24, 48000, 4);
  put(b, 28, 96000, 4);
  put(b, 32, 2, 2);
  put(b, 34, 16, 2);
  std::memcpy(b.data() + 36, "data", 4);
  put(b, 40, count, 4);
  for (unsigned i = 44; i < b.size(); ++i)
    b[i] = std::byte((i * 17 + salt) & 255);
  return b;
}
trg::Asset asset(const std::vector<std::byte> &b) {
  return {b.size(), [&b](std::uint64_t at, std::span<std::byte> into) {
            const auto n = std::min<std::size_t>(
                {into.size(), b.size() - std::size_t(at), 113});
            std::copy_n(b.data() + at, n, into.data());
            return n;
          }};
}
void portable() {
  Folder f;
  const auto a = wave(1, 80000), b = wave(2, 7);
  const std::array assets{asset(a), asset(b)};
  const std::string json = "{\"schema\":\"synthetic.instrument\",\"name\":"
                           "\"Snare Ω\",\"layers\":[0,1,0]}";
  const auto file = f.path / "portable.trg";
  trg::write(file, json, assets);
  const auto bytes = files::readBounded(file, 100000);
  need(std::memcmp(bytes.data(), "CELLTRG1", 8) == 0 && le(bytes, 8, 4) == 1 &&
           le(bytes, 16, 4) == json.size() && le(bytes, 20, 4) == 2 &&
           le(bytes, 32, 8) == bytes.size(),
       "Independent TRG header check failed");
  const auto offset = (40 + json.size() + 32 + 7) & ~std::size_t(7);
  need(le(bytes, unsigned(40 + json.size()), 8) == offset &&
           std::equal(a.begin(), a.end(),
                      bytes.begin() + std::ptrdiff_t(offset)),
       "TRG changed PCM or table offsets");
  std::filesystem::rename(file, f.path / "moved.trg");
  auto source = std::make_shared<trg::Source>(f.path / "moved.trg");
  need(source->metadata() == json && source->entries().size() == 2,
       "Portable metadata changed");
  std::atomic<bool> correct{true};
  std::vector<std::thread> threads;
  for (unsigned worker = 0; worker < 6; ++worker)
    threads.emplace_back([&, worker] {
      const auto n = worker % 2;
      const auto &expected = n ? b : a;
      trg::WaveView view(source, n);
      std::array<std::byte, 257> read{};
      for (unsigned pass = 0; pass < 70; ++pass) {
        const auto at = (pass * 131u) % expected.size();
        if (!view.seek(at)) {
          correct = false;
          return;
        }
        const auto count = view.read(read);
        if (count != std::min(read.size(), expected.size() - at) ||
            !std::equal(read.begin(), read.begin() + std::ptrdiff_t(count),
                        expected.begin() + std::ptrdiff_t(at)))
          correct = false;
      }
      if (!view.seek(view.length()) || view.read(read) ||
          view.seek(view.length() + 1))
        correct = false;
    });
  for (auto &t : threads)
    t.join();
  need(correct, "Concurrent TRG source views crossed cursors");
  rejects([&] { trg::write(f.path / "moved.trg", json, assets); });
  need(files::readBounded(f.path / "moved.trg", 100000) == bytes,
       "Exclusive TRG write replaced a destination");
  unsigned checks{};
  rejects([&] {
    trg::write(f.path / "moved.trg", json, assets, true, [&] {
      if (++checks == 5)
        throw std::runtime_error("cancel");
    });
  });
  need(files::readBounded(f.path / "moved.trg", 100000) == bytes,
       "Cancellation replaced the old TRG");
  auto bad = assets;
  bad[1].read = [](std::uint64_t, std::span<std::byte>) {
    return std::size_t(0);
  };
  rejects([&] { trg::write(f.path / "moved.trg", json, bad, true); });
  need(files::readBounded(f.path / "moved.trg", 100000) == bytes,
       "Failed source preparation changed TRG");
  trg::write(f.path / "empty.trg", "{}", {});
  need(trg::Source(f.path / "empty.trg").entries().empty(),
       "Empty portable kit rejected");
  for (const std::string_view text :
       {"[]", "{\"x\":1,\"x\":2}", "{\"x\":NaN}", "\"x\""})
    rejects([&] { trg::write(f.path / "invalid.trg", text, assets); });
  for (const auto &entry : std::filesystem::directory_iterator(f.path))
    need(!entry.path().filename().string().starts_with(".cell-write-"),
         "TRG temporary output leaked");
}
void malformed() {
  Folder f;
  const auto a = wave(9, 100);
  const std::array assets{asset(a), asset(a)};
  const auto p = f.path / "good.trg";
  trg::write(p, "{}", assets);
  const auto good = files::readBounded(p, 4096);
  const auto bad = f.path / "bad.trg";
  for (std::size_t cut = 0; cut < good.size(); ++cut) {
    files::atomicWrite(bad, std::span(good).first(cut), true);
    rejects([&] { trg::Source s(bad); });
  }
  auto corrupt = [&](unsigned at, std::uint64_t value, unsigned count) {
    auto b = good;
    put(b, at, value, count);
    files::atomicWrite(bad, b, true);
    rejects([&] { trg::Source s(bad); });
  };
  corrupt(8, 2, 4);
  corrupt(12, 1, 4);
  corrupt(16, UINT32_MAX, 4);
  corrupt(20, 65537, 4);
  corrupt(24, UINT64_MAX, 8);
  corrupt(42, 0, 8);
  corrupt(50, UINT64_MAX, 8);
  corrupt(58, le(good, 42, 8), 8);
  corrupt(66, 0, 8);
  auto trailing = good;
  trailing.push_back(std::byte{});
  put(trailing, 32, trailing.size(), 8);
  files::atomicWrite(bad, trailing, true);
  rejects([&] { trg::Source s(bad); });
  bool cancelled{};
  try {
    trg::Source s(p, [&] { throw 42; });
  } catch (int) {
    cancelled = true;
  }
  need(cancelled, "TRG inspection ignored cancellation");
}
void sparse() {
  Folder f;
  const auto p = f.path / "large.trg";
  constexpr std::uint64_t size = (UINT64_C(1) << 32) + 8193;
  std::array<std::byte, 64> prefix{};
  std::memcpy(prefix.data(), "CELLTRG1", 8);
  put(prefix, 8, 1, 4);
  put(prefix, 16, 2, 4);
  put(prefix, 20, 1, 4);
  put(prefix, 24, 64, 8);
  put(prefix, 32, 64 + size, 8);
  prefix[40] = std::byte{'{'};
  prefix[41] = std::byte{'}'};
  put(prefix, 42, 64, 8);
  put(prefix, 50, size, 8);
  {
    std::ofstream out(p, std::ios::binary);
    out.write(reinterpret_cast<const char *>(prefix.data()), prefix.size());
    out.write("RF64\xff\xff\xff\xffWAVE", 12);
    out.seekp(std::streamoff(64 + size - 4));
    out.write("tail", 4);
    need(bool(out), "Sparse TRG fixture creation failed");
  }
  auto source = std::make_shared<trg::Source>(p);
  trg::WaveView view(source, 0);
  std::array<std::byte, 4> last{};
  need(view.length() == size && view.seek(size - 4) && view.read(last) == 4 &&
           std::memcmp(last.data(), "tail", 4) == 0,
       "TRG sample view narrowed a 64-bit position");
}
} // namespace
int main() {
  try {
    portable();
    malformed();
    sparse();
    std::cout << "PASS TRG: exact WAVE bytes, portable metadata, short-read "
                 "streaming, concurrent views, 64-bit sparse seeks, "
                 "malformed/truncated/overlapping input, atomic "
                 "collision/cancellation and cleanup\n";
    return 0;
  } catch (const std::exception &e) {
    std::cerr << e.what() << '\n';
    return 1;
  }
}
