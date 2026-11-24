// SPDX-License-Identifier: Apache-2.0
#include "fixtures/gog_fixture.hpp"
#include <cell/gog.hpp>
#include <chrono>
#include <iostream>
#include <thread>

namespace g = cell::gog;
namespace {
void check(bool ok, const char *why) {
  if (!ok)
    throw std::runtime_error(why);
}
struct Folder {
  std::filesystem::path path =
      std::filesystem::temp_directory_path() /
      ("cell-sdk-gog-" +
       std::to_string(
           std::chrono::steady_clock::now().time_since_epoch().count()));
  Folder() { std::filesystem::create_directories(path); }
  ~Folder() {
    std::error_code error;
    std::filesystem::remove_all(path, error);
  }
};
void views() {
  Folder folder;
  std::array<cell_test::GogSample, 3> samples;
  samples[0].frames = 20001;
  samples[1].frames = 17001;
  samples[1].bits = 24;
  samples[2].frames = 20000;
  samples[2].bits = 24;
  samples[2].channels = 2;
  samples[2].group = 1;
  const auto bytes =
      cell_test::gogBytes(samples, std::array<float, 2>{.1f, .9f});
  const auto file = folder.path / "own.gog";
  cell::files::atomicWrite(file, bytes);
  auto source = std::make_shared<g::Source>(file);
  check(source->program().samples.size() == 3 && source->program().groups == 2,
        "SDK GOG metadata lost samples/groups");
  for (unsigned index = 0; index < samples.size(); ++index) {
    const auto &item = source->program().samples[index];
    g::WaveView view(source, index);
    std::vector<std::byte> decoded(view.length());
    std::size_t at = 0;
    for (std::size_t quantum : {1u, 7u, 4096u, 16384u, 97u}) {
      check(view.seek(0), "SDK GOG seek to start failed");
      at = 0;
      while (at < decoded.size()) {
        const auto n = view.read(std::span(decoded).subspan(
            at, std::min(quantum, decoded.size() - at)));
        check(n != 0, "SDK GOG view ended early");
        at += n;
      }
      check(view.read(std::span(decoded).first(1)) == 0,
            "SDK GOG view passed EOF");
      check(std::equal(decoded.begin() + 44, decoded.begin() + 44 + item.bytes,
                       bytes.begin() + item.offset),
            "SDK GOG view changed PCM or crossed a sample boundary");
      if (item.bytes & 1)
        check(decoded.back() == std::byte{},
              "SDK GOG WAVE padding was not zero");
    }
    for (unsigned at : {43, 44, 45, 4096, 16384}) {
      check(view.seek(at), "SDK GOG random seek failed");
      std::array<std::byte, 19> part{};
      const auto count = view.read(part);
      check(
          std::equal(part.begin(), part.begin() + count, decoded.begin() + at),
          "SDK GOG random read differs from sequential read");
    }
    const auto position = view.position();
    check(!view.seek(view.length() + 1) && view.position() == position,
          "SDK GOG invalid seek changed position");
  }
  // Concurrent views share one container while retaining independent cursors.
  std::array<std::thread, 3> workers;
  std::atomic<bool> good{true};
  for (unsigned index = 0; index < workers.size(); ++index)
    workers[index] = std::thread([&, index] {
      const auto &item = source->program().samples[index];
      g::WaveView view(source, index);
      for (unsigned n = 0; n < 80; ++n) {
        const auto at = 44 + (n * 733) % unsigned(item.bytes - 64);
        std::array<std::byte, 64> part{};
        if (!view.seek(at) || view.read(part) != part.size() ||
            !std::equal(part.begin(), part.end(),
                        bytes.begin() + item.offset + at - 44))
          good = false;
      }
    });
  for (auto &worker : workers)
    worker.join();
  check(good, "Shared GOG source mixed concurrent sample views");
  for (unsigned kind = 0; kind < 8; ++kind) {
    auto broken = bytes;
    if (kind == 0)
      broken.resize(100);
    if (kind == 1)
      broken[0] = std::byte{};
    if (kind == 2)
      broken[33890] = std::byte{32};
    if (kind == 3)
      broken[33506] = std::byte{2};
    if (kind == 4)
      broken[28514] = std::byte{2};
    if (kind == 5)
      broken[14306] = std::byte{31};
    if (kind == 6)
      broken.pop_back();
    if (kind == 7)
      broken.push_back(std::byte{});
    try {
      g::parse(std::span<const std::byte>(broken).first(
                   std::min<std::size_t>(broken.size(), g::headerBytes)),
               broken.size());
      throw std::runtime_error("SDK accepted malformed GOG input");
    } catch (const std::invalid_argument &) {
    }
  }
  std::vector<cell_test::GogSample> maximum(g::maximumSamples);
  for (auto &s : maximum)
    s.frames = 2;
  const auto full = cell_test::gogBytes(maximum, std::array<float, 1>{.5f});
  check(g::parse(std::span<const std::byte>(full).first(g::headerBytes),
                 full.size())
                .samples.size() == 384,
        "SDK GOG reader imposed an incomplete sample limit");
}
} // namespace
int main() {
  try {
    views();
    std::cout << "PASS independent SDK GOG v3.5 PCM views, exact bytes, "
                 "seek/EOF/padding, shared-source concurrency, 384 records and "
                 "malformed layouts\n";
  } catch (const std::exception &error) {
    std::cerr << error.what() << '\n';
    return 1;
  }
}
