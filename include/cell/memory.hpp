// Original Cell Framework implementation; see LICENSE.
#pragma once
#include <cstdint>
#include <optional>
namespace cell {
// Control/loader thread only. Unknown telemetry does not impose a fabricated cap.
std::optional<std::uint64_t> availableMemoryBytes()noexcept;
}
