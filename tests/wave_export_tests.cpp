// Original Cell Framework implementation; see LICENSE.
#include "fixtures/gog_fixture.hpp"
#include "fixtures/tci_fixture.hpp"
#include <cell/tci.hpp>
#include <cell/wave_export.hpp>
#include <chrono>
#include <iostream>

namespace {
void need(bool ok, const char *message) {
  if (!ok)
    throw std::runtime_error(message);
}
struct Folder {
  std::filesystem::path path =
      std::filesystem::temp_directory_path() /
      ("cell-wave-export-" +
       std::to_string(
           std::chrono::steady_clock::now().time_since_epoch().count()));
  Folder() { std::filesystem::create_directories(path); }
  ~Folder() {
    std::error_code ec;
    std::filesystem::remove_all(path, ec);
  }
};
template <class View>
void exercise(View &view, const std::filesystem::path &path) {
  std::vector<std::byte> expected(view.length());
  need(view.read(expected) == expected.size(), "Expected view ended early");
  need(view.seek(17), "Export fixture seek failed");
  cell::audiofiles::exportWaveView(view, path);
  need(cell::files::readBounded(path, expected.size()) == expected,
       "Export changed WAVE header or PCM bytes");
  bool failed{};
  try {
    cell::audiofiles::exportWaveView(view, path);
  } catch (const std::runtime_error &) {
    failed = true;
  }
  need(failed && cell::files::readBounded(path, expected.size()) == expected,
       "Exclusive export replaced the previous file");
  const std::array<std::byte, 3> sentinel{std::byte{7}, std::byte{8},
                                          std::byte{9}};
  cell::files::atomicWrite(path, sentinel, true);
  unsigned calls{};
  failed = false;
  try {
    cell::audiofiles::exportWaveView(view, path, true, [&] {
      if (++calls == 3)
        throw std::runtime_error("cancel");
    });
  } catch (const std::runtime_error &) {
    failed = true;
  }
  need(failed && calls == 3 &&
           cell::files::readBounded(path, 3) ==
               std::vector(sentinel.begin(), sentinel.end()),
       "Cancelled export published partial bytes");
  cell::audiofiles::exportWaveView(view, path, true);
  need(cell::files::readBounded(path, expected.size()) == expected,
       "Explicit replacement changed audio");
  struct Broken {
    bool seek(std::uint64_t) { return true; }
    std::uint64_t length() const { return 65537; }
    std::size_t read(std::span<std::byte> bytes) {
      return bytes.size() == 1 ? 0 : bytes.size();
    }
  } broken;
  failed = false;
  try {
    cell::audiofiles::exportWaveView(broken, path, true);
  } catch (const std::runtime_error &) {
    failed = true;
  }
  need(failed && cell::files::readBounded(path, expected.size()) == expected,
       "Decoder failure replaced completed export");
}
} // namespace
int main() {
  try {
    Folder folder;
    std::array<cell_test::GogSample, 1> samples;
    samples[0].bits = 24;
    samples[0].channels = 2;
    samples[0].frames = 16001;
    const auto gog = folder.path / "own.gog";
    cell_test::writeGog(gog, samples, std::array<float, 1>{.5f});
    cell::gog::WaveView gogView(std::make_shared<cell::gog::Source>(gog), 0);
    exercise(gogView, folder.path / "gog.wav");
    std::array<cell_test::TciSample, 1> tcis;
    tcis[0].channels = 2;
    tcis[0].scalars.resize(32002);
    for (unsigned n = 0; n < tcis[0].scalars.size(); ++n)
      tcis[0].scalars[n] = int(n) * 127 - 2000000;
    const auto tci = folder.path / "own.tci";
    cell::files::atomicWrite(tci, cell_test::tciBytes(tcis));
    cell::tci::WaveView tciView(std::make_shared<cell::tci::Source>(tci), 0);
    exercise(tciView, folder.path / "tci.wav");
    std::size_t files{};
    for (const auto &entry : std::filesystem::directory_iterator(folder.path))
      if (entry.is_regular_file())
        ++files;
    need(files == 4, "Export cancellation/failure left temporary files");
    std::cout
        << "PASS explicit GOG/TCI WAVE export: exact bytes, streaming, rewind, "
           "exclusive/replace, cancellation and decoder-failure publication\n";
    return 0;
  } catch (const std::exception &error) {
    std::cerr << error.what() << '\n';
    return 1;
  }
}
