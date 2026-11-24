// Original Cell Framework implementation; see LICENSE.
#pragma once
#include <algorithm>
#include <array>
#include <cell/files.hpp>
#include <functional>
#include <stdexcept>

namespace cell::audiofiles {
using ExportWorkCheck = std::function<void()>;
// Export a validated seekable WAVE view, such as gog::WaveView or
// tci::WaveView. The worker streams fixed-size blocks, preserving
// rate/channels/PCM bytes. Cancellation throws from check; failure retains the
// previous destination. Publication is exclusive unless replace is explicitly
// true. This API does not normalize, trim, dither, resample or build an
// instrument mapping.
template <class View>
void exportWaveView(View &view, const std::filesystem::path &destination,
                    bool replace = false, const ExportWorkCheck &check = {}) {
  if (check)
    check();
  if (!view.seek(0))
    throw std::invalid_argument("Cannot rewind WAVE export source");
  files::AtomicWriter output(destination, replace);
  std::array<std::byte, 65536> buffer{};
  auto remaining = view.length();
  while (remaining) {
    if (check)
      check();
    const auto requested =
        std::size_t(std::min<std::uint64_t>(remaining, buffer.size()));
    const auto count = view.read(std::span(buffer).first(requested));
    if (count == 0 || count > requested)
      throw std::runtime_error("WAVE export source ended early");
    output.write(std::span(buffer).first(count));
    remaining -= count;
  }
  if (check)
    check();
  output.commit();
}
} // namespace cell::audiofiles
