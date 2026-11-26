// Original Cell Framework implementation; see LICENSE.
#include <cell/dynamic_library.hpp>
#include <clap/clap.h>
#include <clap/ext/preset-load.h>
#include <array>
#include <algorithm>
#include <iostream>

namespace {
void expect(bool accepted, const char* reason) {
    if (!accepted) throw std::runtime_error(reason);
}
}

int main(int argc, char** argv) try {
    expect(argc == 3, "Expected module and bundle paths");
    cell::DynamicLibrary library;
    expect(library.open(argv[1]), "Could not open the banked module");
    const auto* entry = static_cast<const clap_plugin_entry_t*>(library.getFunction("clap_entry"));
    expect(entry && entry->init(argv[2]), "Banked CLAP entry failed");
    const auto* factory = static_cast<const clap_plugin_factory_t*>(entry->get_factory(CLAP_PLUGIN_FACTORY_ID));
    expect(factory && factory->get_plugin_count(factory) == 1, "Missing CLAP factory");
    const clap_host_t host{CLAP_VERSION, nullptr, "Cell fixture host", "Cell", "", "1",
        [](const clap_host_t*, const char*) -> const void* { return nullptr; },
        [](const clap_host_t*) {}, [](const clap_host_t*) {}, [](const clap_host_t*) {}};
    const auto* plugin = factory->create_plugin(factory, &host, factory->get_plugin_descriptor(factory, 0)->id);
    expect(plugin && plugin->init(plugin), "Could not initialize the banked CLAP plug-in");
    const auto* params = static_cast<const clap_plugin_params_t*>(plugin->get_extension(plugin, CLAP_EXT_PARAMS));
    const auto* presets = static_cast<const clap_plugin_preset_load_t*>(plugin->get_extension(plugin, CLAP_EXT_PRESET_LOAD));
    clap_param_info_t info{};
    double value{};
    expect(params && params->get_info(plugin, 0, &info), "Missing gain parameter");
    expect(presets && !presets->from_location(plugin, CLAP_PRESET_DISCOVERY_LOCATION_PLUGIN, nullptr, "2:99") &&
           presets->from_location(plugin, CLAP_PRESET_DISCOVERY_LOCATION_PLUGIN, nullptr, "2:4") &&
           params->get_value(plugin, info.id, &value) && value == -28,
           "CLAP preset recall did not select the requested bank and slot");
    expect(plugin->activate(plugin, 48000, 1, 32) &&
           plugin->start_processing(plugin), "Could not start the banked CLAP plug-in");

    constexpr std::array<std::uint8_t, 8> recall{0xf0,0x7d,0x43,0x47,1,1,2,0xf7};
    constexpr std::array<std::uint8_t, 7> request{0xf0,0x7d,0x43,0x47,2,1,0xf7};
    constexpr std::array<std::uint8_t, 13> expected{0xf0,0x7d,0x43,0x47,3,1,12,14,16,18,20,22,0xf7};
    std::array<clap_event_midi_sysex_t, 2> incoming{{
        {{sizeof(clap_event_midi_sysex_t), 3, CLAP_CORE_EVENT_SPACE_ID, CLAP_EVENT_MIDI_SYSEX, 0},
         0, recall.data(), recall.size()},
        {{sizeof(clap_event_midi_sysex_t), 19, CLAP_CORE_EVENT_SPACE_ID, CLAP_EVENT_MIDI_SYSEX, 0},
         0, request.data(), request.size()}
    }};
    clap_input_events_t input{&incoming,
        [](const clap_input_events_t*) -> std::uint32_t { return 2; },
        [](const clap_input_events_t* events, std::uint32_t index) -> const clap_event_header_t* {
            const auto& items = *static_cast<decltype(incoming)*>(events->ctx);
            return index < items.size() ? &items[index].header : nullptr;
        }};
    struct Capture { std::array<std::uint8_t, 13> bytes{}; unsigned count{}, time{}, port{}; } receipt;
    clap_output_events_t output{&receipt,
        [](const clap_output_events_t* events, const clap_event_header_t* header) {
            if (header->type != CLAP_EVENT_MIDI_SYSEX) return true;
            const auto& event = *reinterpret_cast<const clap_event_midi_sysex_t*>(header);
            auto& capture = *static_cast<Capture*>(events->ctx);
            if (!event.buffer || event.size != capture.bytes.size()) return false;
            std::copy_n(event.buffer, event.size, capture.bytes.begin());
            capture.time = header->time;
            capture.port = event.port_index;
            ++capture.count;
            return true;
        }};
    std::array<float, 32> left{}, right{}, dryLeft{}, dryRight{};
    dryLeft.fill(1);
    dryRight.fill(1);
    float* channels[]{left.data(), right.data()};
    float* dry[]{dryLeft.data(), dryRight.data()};
    clap_audio_buffer_t out{channels, nullptr, 2};
    clap_audio_buffer_t in{dry, nullptr, 2};
    clap_process_t block{0, 32, nullptr, &in, &out, 1, 1, &input, &output};
    const auto processed = plugin->process(plugin, &block);
    const bool recalled = params->get_value(plugin, info.id, &value) && value == -44;
    plugin->reset(plugin);
    clap_event_param_value_t automation{};
    automation.header = {sizeof(automation), 8, CLAP_CORE_EVENT_SPACE_ID, CLAP_EVENT_PARAM_VALUE, 0};
    automation.param_id = info.id;
    automation.note_id = automation.port_index = automation.channel = automation.key = -1;
    automation.value = -6;
    const clap_input_events_t edits{&automation,
        [](const clap_input_events_t*) -> std::uint32_t { return 1; },
        [](const clap_input_events_t* events, std::uint32_t) -> const clap_event_header_t* {
            return &static_cast<clap_event_param_value_t*>(events->ctx)->header;
        }};
    block.steady_time = 32;
    block.in_events = &edits;
    expect(plugin->process(plugin, &block) != CLAP_PROCESS_ERROR &&
           std::abs(left[7] - left[0]) < 1e-8f && left[8] > left[7] && left.back() > left[8] &&
           params->get_value(plugin, info.id, &value) && value == -6,
           "CLAP automation did not reach the processor at its sample offset");
    plugin->stop_processing(plugin);
    plugin->deactivate(plugin);
    plugin->destroy(plugin);
    entry->deinit();
    expect(processed != CLAP_PROCESS_ERROR && recalled, "CLAP SysEx recall did not reach gain");
    expect(receipt.count == 1 && receipt.port == 0 && receipt.time == 19 && receipt.bytes == expected,
           "CLAP bank dump bytes, port or sample offset differ");
} catch (const std::exception& failure) {
    std::cerr << failure.what() << '\n';
    return 1;
}
