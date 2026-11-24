// Original Cell Framework implementation; see LICENSE.
#pragma once

#include <cell/web_parameter_bridge.hpp>
#include <lv2/core/lv2.h>

namespace cell::lv2 {
inline constexpr const char* programAccessUri = "https://cell-framework.dev/lv2/editor-program-access/v1";

// Private exchange within the matching DSP and UI modules. The host supplies
// both instance-access and data-access when the UI shares the DSP process.
struct ProgramAccess {
    std::optional<ProgramCatalog> (*programs)(LV2_Handle);
    bool (*recall)(LV2_Handle, ProgramAddress, std::vector<ParameterEdit>&);
    bool (*supports)(LV2_Handle, std::string_view, web::ActionMode);
    bool (*action)(LV2_Handle, std::string_view, web::ActionPhase, std::vector<ParameterEdit>&);
};
}
