// Original Cell Framework implementation; see LICENSE.
#pragma once

#include <AudioToolbox/AudioToolbox.h>
#include <atomic>
#include <memory>
#include <mutex>
#include <cell/web_parameter_bridge.hpp>
#if defined(CELL_AU_EDITOR_METERS)
#include <cell/web_meter.hpp>
#endif

namespace cell::au {
struct EditorConnection {
    explicit EditorConnection(AudioUnit instance) : unit(instance) {
#if defined(CELL_AU_EDITOR_METERS)
        meter.setEnabled(true);
#endif
    }

    std::atomic<AudioUnit> unit;
    std::mutex commandMutex;
    std::function<std::optional<ProgramCatalog>()> programs;
    std::function<bool(std::string_view, web::ActionMode)> supportsAction;
    std::function<bool(const std::function<bool(Engine&)>&, std::vector<ParameterEdit>&)> command;
#if defined(CELL_AU_EDITOR_METERS)
    std::atomic<unsigned> meterReaders{};
    web::OutputMeter meter;
#endif
};

std::shared_ptr<EditorConnection> registerEditorConnection(AudioUnit unit);
void unregisterEditorConnection(AudioUnit unit);
OSStatus copyCocoaViewInfo(AudioUnitCocoaViewInfo& info);
}
