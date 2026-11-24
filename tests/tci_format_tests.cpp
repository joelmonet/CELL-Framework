// Original Cell Framework implementation; see LICENSE.
#include "allocation_watch.hpp"
#include "fixtures/tci_fixture.hpp"
#include <atomic>
#include <cell/files.hpp>
#include <cell/tci.hpp>
#include <chrono>
#include <iostream>
#include <random>
#include <thread>

namespace {
void need(bool value, const char *message) {
  if (!value)
    throw std::runtime_error(message);
}
struct Folder {
  std::filesystem::path path =
      std::filesystem::temp_directory_path() /
      ("cell-tci-" +
       std::to_string(
           std::chrono::steady_clock::now().time_since_epoch().count()));
  Folder() { std::filesystem::create_directories(path); }
  ~Folder() {
    std::error_code ec;
    std::filesystem::remove_all(path, ec);
  }
};
template <class Work> void rejects(Work work) {
  try {
    work();
  } catch (const std::invalid_argument &) {
    return;
  }
  throw std::runtime_error("Malformed TCI input was accepted");
}
std::vector<std::byte> pcm(const cell_test::TciSample &sample) {
  std::vector<std::byte> result(sample.scalars.size() * 3);
  for (std::size_t n = 0; n + 1 < sample.scalars.size(); ++n)
    for (unsigned byte = 0; byte < 3; ++byte)
      result[n * 3 + byte] =
          std::byte((std::uint32_t(sample.scalars[n]) >> (byte * 8)) & 255);
  return result;
}
void run() {
  Folder folder;
  const auto file = folder.path / "own.tci";
  std::array<cell_test::TciSample, 3> samples;
  samples[0].scalars = {1, -2, 3, 777};
  samples[1].channels = 2;
  samples[1].name = "stereo";
  samples[1].scalars.resize(1000);
  for (unsigned n = 0; n < samples[1].scalars.size(); ++n)
    samples[1].scalars[n] = n < 201   ? 0
                            : n < 603 ? int(n * 701) - 300000
                            : n % 2   ? -8388608
                                      : 8388607;
  samples[2].scalars.resize(603, -8191);
  samples[2].name = "odd mono";
  const auto bytes = cell_test::tciBytes(samples);
  cell::files::atomicWrite(file, bytes);
  auto source = std::make_shared<cell::tci::Source>(file);
  need(source->program().rate == 48000 && source->program().samples.size() == 3,
       "TCI dimensions changed");
  // Hand-calculated first sample: width 3; sign-magnitude 001,110,011.
  const auto start = source->program().samples[0].packedOffset;
  need(bytes[start] == std::byte{3} && bytes[start + 1] == std::byte{0x39} &&
           bytes[start + 2] == std::byte{0x80},
       "Golden TCI bit fixture changed");
  bool unknown{};
  for (const auto &record : source->program().records)
    if (record.tag == 789) {
      std::array<std::byte, 4> raw{};
      need(source->read(record.offset, raw) && raw[0] == std::byte{0x39},
           "Unknown TCI metadata was lost");
      unknown = true;
    }
  need(unknown, "Unknown TCI record disappeared");
  std::array<std::vector<std::byte>, 3> waves;
  for (unsigned index = 0; index < samples.size(); ++index) {
    cell::tci::WaveView view(source, index);
    auto &wave = waves[index];
    wave.resize(view.length());
    const auto expected = pcm(samples[index]);
    for (unsigned quantum : {1u, 7u, 602u, 603u, 4096u}) {
      need(view.seek(0), "TCI start seek failed");
      std::size_t at{};
      while (at < wave.size()) {
        const auto count = view.read(std::span(wave).subspan(
            at, std::min<std::size_t>(quantum, wave.size() - at)));
        need(count != 0, "TCI view ended early");
        at += count;
      }
      need(std::equal(expected.begin(), expected.end(), wave.begin() + 44),
           "TCI PCM changed sign, block/channel order or absent final value");
      need(view.read(std::span(wave).first(1)) == 0, "TCI read passed EOF");
      if (expected.size() & 1)
        need(wave.back() == std::byte{}, "TCI RIFF pad byte was not zero");
    }
    for (std::uint64_t at : {0u, 43u, 44u, 45u, 647u, 1048u}) {
      if (at >= view.length())
        continue;
      need(view.seek(at), "TCI byte seek failed");
      std::array<std::byte, 100> part{};
      const auto n = view.read(part);
      need(std::equal(part.begin(), part.begin() + n,
                      wave.begin() + std::ptrdiff_t(at)),
           "TCI partial read differed from sequential bytes");
    }
    const auto position = view.position();
    need(!view.seek(view.length() + 1) && view.position() == position,
         "Invalid TCI seek changed cursor");
  }
  std::atomic<bool> good = true;
  std::array<std::thread, 3> workers;
  for (unsigned index = 0; index < workers.size(); ++index)
    workers[index] = std::thread([&, index] {
      try {
        cell::tci::WaveView view(source, index);
        for (unsigned n = 0; n < 100; ++n) {
          const auto at = std::uint64_t(n * 103) % view.length();
          std::array<std::byte, 64> part{};
          if (!view.seek(at)) {
            good = false;
            break;
          }
          const auto count = view.read(part);
          if (!std::equal(part.begin(), part.begin() + count,
                          waves[index].begin() + std::ptrdiff_t(at)))
            good = false;
        }
      } catch (...) {
        good = false;
      }
    });
  for (auto &worker : workers)
    worker.join();
  need(good, "TCI shared source mixed concurrent views");
  cell::tci::WaveView fixed(source, 1);
  std::array<std::byte, 1024> output{};
  cell_test::allocations = 0;
  cell_test::audio = true;
  fixed.read(output);
  fixed.seek(645);
  fixed.read(output);
  cell_test::audio = false;
  need(cell_test::allocations == 0, "TCI view read allocated scratch memory");
  // Format IO remains worker-only despite its fixed scratch space.
  const auto corruptFile = folder.path / "bad.tci";
  const auto small = cell_test::tciBytes(std::span(samples).first(1));
  for (std::size_t count = 0; count < small.size(); ++count) {
    cell::files::atomicWrite(corruptFile, std::span(small).first(count), true);
    rejects([&] { cell::tci::Source value(corruptFile); });
  }
  for (unsigned kind = 0; kind < 7; ++kind) {
    auto bad = small;
    if (kind == 0)
      bad[72] = std::byte{3}; // Version.
    if (kind == 1)
      bad[start - 9] = std::byte{3}; // Channels.
    if (kind == 2)
      bad[start] = std::byte{}; // Width.
    if (kind == 3)
      bad[start - 8] = std::byte{1}; // Bit budget.
    if (kind == 4)
      bad[start - 4] = std::byte{0xff}; // Scalar count.
    if (kind == 5)
      bad[start + 3] = std::byte{}; // Label.
    if (kind == 6)
      bad.push_back(std::byte{}); // Data after end marker.
    cell::files::atomicWrite(corruptFile, bad, true);
    rejects([&] { cell::tci::Source value(corruptFile); });
  }
  cell::files::atomicWrite(corruptFile, small, true);
  rejects([&] { cell::tci::Source value(corruptFile, {.records = 2}); });
  rejects([&] { cell::tci::Source value(corruptFile, {.samples = 0}); });
  rejects([&] { cell::tci::Source value(corruptFile, {.labelBytes = 2}); });
  std::mt19937 random(8271);
  for (unsigned n = 0; n < 2000; ++n) {
    auto bad = small;
    bad[random() % bad.size()] ^= std::byte(1u << (random() % 8));
    cell::files::atomicWrite(corruptFile, bad, true);
    try {
      auto mutated = std::make_shared<cell::tci::Source>(corruptFile);
      for (unsigned i = 0; i < mutated->program().samples.size(); ++i) {
        cell::tci::WaveView view(mutated, i);
        while (view.read(output)) {
        }
      }
    } catch (const std::invalid_argument &) {
    }
  }
}
} // namespace
int main() {
  try {
    run();
    std::cout
        << "PASS TCI v4 embedded PCM: fixed sign-magnitude fixture, "
           "block/channel order, final-scalar padding, byte seeks, shared "
           "views, bounded scratch, truncation, admission and 2000 mutations\n";
    return 0;
  } catch (const std::exception &error) {
    cell_test::audio = false;
    std::cerr << error.what() << '\n';
    return 1;
  }
}
