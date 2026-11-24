// Original Cell Framework implementation; see LICENSE.
#pragma once
#include "pluginterfaces/base/funknown.h"
#include <cstdint>

namespace cell::vst3 {
// ABI documented by Cockos in reaper_vst3_interfaces.h. Query only on a host
// control thread. The returned identity is opaque and scoped to this process;
// never persist or dereference it. Keep projects distinct even when names
// match.
class ReaperApplication : public Steinberg::FUnknown {
public:
  virtual void *PLUGIN_API reaperApi(Steinberg::CStringA) = 0;
  virtual void *PLUGIN_API reaperParent(Steinberg::uint32) = 0;
  virtual void *PLUGIN_API reaperExtended(Steinberg::uint32, void *, void *,
                                          void *) = 0;
};
// A project may be unavailable during initialize. A format adapter that binds
// ARA later can query again from its already-owned VST3 host context before
// first audio activation. No host pointer is retained here and every acquired
// interface is released.
inline std::uint64_t projectIdentity(Steinberg::FUnknown *context) {
  const Steinberg::TUID id =
      INLINE_UID(0x79655E36, 0x77EE4267, 0xA573FEF7, 0x4912C27C);
  void *result = nullptr;
  if (!context ||
      context->queryInterface(id, &result) != Steinberg::kResultOk || !result)
    return 0;
  auto *application = static_cast<ReaperApplication *>(result);
  const auto project = std::uint64_t(
      reinterpret_cast<std::uintptr_t>(application->reaperParent(3)));
  application->release();
  return project;
}
} // namespace cell::vst3
