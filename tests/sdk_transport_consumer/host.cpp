// SPDX-License-Identifier: Apache-2.0
#include <algorithm>
#include <array>
#include <bit>
#include <cstdint>
#include <cmath>
#include <iostream>
#include <limits>
#include <stdexcept>
#include <string>
#if CELL_TRANSPORT_CLAP
#include <clap/clap.h>
#include <cell/realtime.hpp>
#include <cstring>
#if defined(_WIN32)
#include <windows.h>
#else
#include <dlfcn.h>
#endif
#else
#include "pluginterfaces/vst/ivstaudioprocessor.h"
#include "pluginterfaces/vst/ivstprocesscontext.h"
#include "public.sdk/source/vst/hosting/eventlist.h"
#include "public.sdk/source/vst/hosting/hostclasses.h"
#include "public.sdk/source/vst/hosting/module.h"
#include "public.sdk/source/vst/hosting/parameterchanges.h"
#include "public.sdk/source/vst/hosting/plugprovider.h"
#endif
namespace {
void check(bool b, const char *why) {
  if (!b)
    throw std::runtime_error(why);
}
using Channels = std::array<std::array<float, 64>, 2>;
void output(const Channels &values, bool valid) {
  for (unsigned n = 0; n < 64; ++n)
    check(
        values[0][n] == (valid ? 137.5f : 120.f) &&
            values[1][n] == (valid ? 1.f : 0.f),
        "Actual adapter lost tempo validity, fallback or segment propagation");
}
const std::array bpms{137.5, 0., -10., std::numeric_limits<double>::quiet_NaN(),
                      std::numeric_limits<double>::infinity()};
constexpr auto sysExPayload = [] {
  std::array<std::uint8_t, 4099> bytes{};
  bytes.front() = 0xf0;
  bytes[1] = 0x7d;
  for (std::size_t i = 2; i < bytes.size() - 1; ++i) bytes[i] = std::uint8_t(i % 128);
  bytes.back() = 0xf7;
  return bytes;
}();
#if CELL_TRANSPORT_CLAP
void run(const char *path) {
#if defined(_WIN32)
  auto library = LoadLibraryA(path);
  auto *entry = library ? reinterpret_cast<const clap_plugin_entry_t *>(
                              GetProcAddress(library, "clap_entry"))
                        : nullptr;
#else
  auto *library = dlopen(path, RTLD_NOW | RTLD_LOCAL);
  auto *entry = library ? static_cast<const clap_plugin_entry_t *>(
                              dlsym(library, "clap_entry"))
                        : nullptr;
#endif
  check(entry && entry->init(path), "Transport CLAP load failed");
  struct TimingObservation {
    const clap_plugin_t *plugin{};
    const clap_plugin_latency_t *extension{};
    std::uint32_t observed{};
    unsigned changes{}, restarts{};
    const clap_plugin_tail_t *tail{};
    std::uint32_t observedTail{};
    unsigned tailChanges{};
  } latency;
  static const clap_host_latency_t hostLatency{[](const clap_host_t *host) {
    auto &state = *static_cast<TimingObservation *>(host->host_data);
    ++state.changes;
    state.observed = state.extension->get(state.plugin);
  }};
  static const clap_host_tail_t hostTail{[](const clap_host_t *host) {
    auto &state = *static_cast<TimingObservation *>(host->host_data);
    ++state.tailChanges;
    state.observedTail = state.tail->get(state.plugin);
  }};
  const clap_host_t host{
      CLAP_VERSION,
      &latency,
      "Cell transport host",
      "Cell",
      "",
      "1",
      [](const clap_host_t *, const char *id) -> const void * {
        if (std::strcmp(id, CLAP_EXT_LATENCY) == 0)
          return &hostLatency;
        if (std::strcmp(id, CLAP_EXT_TAIL) == 0)
          return &hostTail;
        return nullptr;
      },
      [](const clap_host_t *host) {
        ++static_cast<TimingObservation *>(host->host_data)->restarts;
      },
      [](const clap_host_t *) {},
      [](const clap_host_t *) {}};
  auto *factory = static_cast<const clap_plugin_factory_t *>(
      entry->get_factory(CLAP_PLUGIN_FACTORY_ID));
  check(factory && factory->get_plugin_count(factory) == 1,
        "Transport CLAP factory missing");
  auto *plugin = factory->create_plugin(
      factory, &host, factory->get_plugin_descriptor(factory, 0)->id);
  check(plugin && plugin->init(plugin), "Transport CLAP init failed");
  latency.plugin = plugin;
  latency.extension = static_cast<const clap_plugin_latency_t *>(plugin->get_extension(plugin, CLAP_EXT_LATENCY));
  latency.tail = static_cast<const clap_plugin_tail_t *>(plugin->get_extension(plugin, CLAP_EXT_TAIL));
  check(latency.extension && latency.tail && plugin->activate(plugin, 48000, 1, 64) &&
            plugin->start_processing(plugin),
        "Transport CLAP lifecycle failed");
  check(latency.changes == 1 && latency.observed == 32, "Host did not see prepared CLAP latency");
  check(latency.tailChanges == 0 && latency.tail->get(plugin) == 128,
        "CLAP activation did not publish the initial tail without a callback");
  Channels input{}, values{};
  std::array<float *, 2> ins{input[0].data(), input[1].data()},
      outs{values[0].data(), values[1].data()};
  clap_audio_buffer_t in{}, out{};
  in.data32 = ins.data();
  out.data32 = outs.data();
  in.channel_count = out.channel_count = 2;
  clap_event_param_value_t event{};
  event.header = {sizeof(event), 17, CLAP_CORE_EVENT_SPACE_ID,
                  CLAP_EVENT_PARAM_VALUE, 0};
  event.param_id = 17;
  event.note_id = -1;
  event.channel = event.key = -1;
  event.value = .5;
  clap_input_events_t events{
      &event, [](const clap_input_events_t *) -> std::uint32_t { return 1; },
      [](const clap_input_events_t *e,
         std::uint32_t n) -> const clap_event_header_t * {
        return n ? nullptr
                 : &static_cast<const clap_event_param_value_t *>(e->ctx)
                        ->header;
      }};
  clap_process_t process{};
  process.frames_count = 64;
  process.audio_inputs = &in;
  process.audio_outputs = &out;
  process.audio_inputs_count = process.audio_outputs_count = 1;
  process.in_events = &events;
  for (bool flag : {false, true})
    for (double bpm : bpms) {
      clap_event_transport_t transport{};
      transport.flags = flag ? CLAP_TRANSPORT_HAS_TEMPO : 0;
      transport.tempo = bpm;
      process.transport = &transport;
      check(plugin->process(plugin, &process) != CLAP_PROCESS_ERROR,
            "Transport CLAP render failed");
      output(values, flag && std::isfinite(bpm) && bpm > 0);
    }
  {
    clap_event_transport_t transport{};
    transport.tempo = 137.5;
    transport.flags = CLAP_TRANSPORT_HAS_TEMPO;
    transport.song_pos_beats = -CLAP_BEATTIME_FACTOR / 4;
    process.transport = &transport;
    check(plugin->process(plugin, &process) != CLAP_PROCESS_ERROR && values[0][0] == 137.5f,
          "CLAP used a beat position without a valid host timeline");
    transport.flags |= CLAP_TRANSPORT_HAS_BEATS_TIMELINE;
    check(plugin->process(plugin, &process) != CLAP_PROCESS_ERROR && values[0][0] == -.25f,
          "CLAP did not pass a valid negative beat position");
    process.transport = nullptr;
  }
  const auto *notePorts = static_cast<const clap_plugin_note_ports_t *>(
      plugin->get_extension(plugin, CLAP_EXT_NOTE_PORTS));
  clap_note_port_info_t midiPort{};
  check(notePorts && notePorts->count(plugin, false) == 1 &&
            notePorts->get(plugin, 0, false, &midiPort) &&
            midiPort.preferred_dialect == CLAP_NOTE_DIALECT_MIDI,
        "CLAP MIDI output port is missing");
  clap_event_midi_t note{};
  note.header = {sizeof(note), 31, CLAP_CORE_EVENT_SPACE_ID, CLAP_EVENT_MIDI, 0};
  note.data[0] = 0x92;
  note.data[1] = 60;
  note.data[2] = 100;
  constexpr std::array<std::uint8_t, 3> malformed{0xf0, 0x80, 0xf7};
  clap_event_midi_sysex_t sysEx{};
  sysEx.header = {sizeof(sysEx), 23, CLAP_CORE_EVENT_SPACE_ID, CLAP_EVENT_MIDI_SYSEX, 0};
  sysEx.buffer = sysExPayload.data();
  sysEx.size = sysExPayload.size();
  auto badSysEx = sysEx;
  badSysEx.header.time = 45;
  badSysEx.buffer = malformed.data();
  badSysEx.size = malformed.size();
  using Incoming = std::array<const clap_event_header_t *, 4>;
  Incoming incoming{&event.header, &sysEx.header, &note.header, &badSysEx.header};
  const clap_input_events_t midiInput{
      &incoming,
      [](const clap_input_events_t *) -> std::uint32_t { return 4; },
      [](const clap_input_events_t *list, std::uint32_t index) {
        const auto &items = *static_cast<const Incoming *>(list->ctx);
        return items[index];
      }};
  struct CapturedMidi {
    std::array<clap_event_midi_t, 49> events{};
    unsigned count{};
    unsigned limit{49};
    std::array<std::uint8_t, sysExPayload.size()> sysEx{};
    unsigned sysExCount{}, sysExFrame{};
  } captured;
  const clap_output_events_t midiOutput{&captured,
      [](const clap_output_events_t *list, const clap_event_header_t *header) {
        auto &capture = *static_cast<CapturedMidi *>(list->ctx);
        if (header->type == CLAP_EVENT_MIDI_SYSEX) {
          const auto& message = *reinterpret_cast<const clap_event_midi_sysex_t *>(header);
          if (message.size != capture.sysEx.size() || message.port_index != 0 || !message.buffer)
            return false;
          ++capture.sysExCount;
          capture.sysExFrame = header->time;
          std::copy_n(message.buffer, message.size, capture.sysEx.begin());
          return true;
        }
        if (header->type != CLAP_EVENT_MIDI) return true;
        if (capture.count == capture.limit) return false;
        capture.events[capture.count++] = *reinterpret_cast<const clap_event_midi_t *>(header);
        return true;
      }};
  process.transport = nullptr;
  process.in_events = &midiInput;
  process.out_events = &midiOutput;
  check(plugin->process(plugin, &process) != CLAP_PROCESS_ERROR && captured.count == 1 &&
            captured.events[0].header.time == 31 && captured.events[0].port_index == 0 &&
            std::equal(note.data, note.data + 3, captured.events[0].data),
        "CLAP lost the routed MIDI event or its block-relative sample offset");
  check(captured.sysExCount == 1 && captured.sysExFrame == 23 && captured.sysEx == sysExPayload,
        "CLAP changed a SysEx payload or timestamp, or forwarded malformed SysEx");
  incoming = {&event.header, &note.header, nullptr, nullptr};
  // Reject a note-off after the previous note-on reached the host.
  note.data[0] = 0x82;
  note.data[2] = 0;
  captured.count = captured.limit = 0;
  check(plugin->process(plugin, &process) != CLAP_PROCESS_ERROR && captured.count == 0,
        "CLAP MIDI backpressure interrupted audio processing");

  const auto checkCleanup = [&](unsigned first, unsigned count) {
    static constexpr std::array controls{64, 120, 123};
    for (unsigned n = 0; n < count; ++n) {
      const auto &message = captured.events[n];
      const auto index = first + n;
      check(message.header.time == 0 && message.port_index == 0 &&
                message.data[0] == (0xb0 | (index / 3)) &&
                message.data[1] == controls[index % 3] && message.data[2] == 0,
            "CLAP MIDI cleanup was lost, duplicated, or reordered");
    }
  };
  note.data[0] = 0x92;
  note.data[2] = 100;
  captured.limit = 7;
  check(plugin->process(plugin, &process) != CLAP_PROCESS_ERROR && captured.count == 7,
        "CLAP did not fill the bounded host queue with cleanup events");
  checkCleanup(0, 7);

  captured.count = 0;
  captured.limit = 49;
  check(plugin->process(plugin, &process) != CLAP_PROCESS_ERROR && captured.count == 42,
        "CLAP did not resume cleanup and deliver the next note");
  checkCleanup(7, 41);
  check(captured.events[41].header.time == 31 &&
            std::equal(note.data, note.data + 3, captured.events[41].data),
        "CLAP changed the next note or delivered it before cleanup");
  process.in_events = &events;
  process.out_events = nullptr;
  process.transport = nullptr;
  check(plugin->process(plugin, &process) != CLAP_PROCESS_ERROR,
        "Transport CLAP missing context render failed");
  output(values, false);
  event.value = .75;
  for (unsigned block = 0; block < 2; ++block)
    check(plugin->process(plugin, &process) != CLAP_PROCESS_ERROR, "CLAP latency-change render failed");
  check(latency.restarts == 1 && latency.changes == 1 && latency.extension->get(plugin) == 32,
        "Active CLAP latency changed or requested more than one restart");
  check(latency.tailChanges == 1 && latency.observedTail == 256,
        "CLAP tail change was missing, repeated, or published after notification");
  plugin->reset(plugin);
  check(latency.tailChanges == 2 && latency.observedTail == 0,
        "CLAP reset did not publish the changed tail");
  check(plugin->process(plugin, &process) != CLAP_PROCESS_ERROR &&
            latency.tailChanges == 3 && latency.observedTail == 256,
        "CLAP processing did not restore tail reporting after reset");
  plugin->stop_processing(plugin);
  plugin->deactivate(plugin);
  check(plugin->activate(plugin, 48000, 1, 64), "CLAP reactivation failed");
  check(latency.changes == 2 && latency.observed == 64 && latency.extension->get(plugin) == 64,
        "CLAP reactivation did not publish new latency");
  check(latency.tailChanges == 3 && latency.tail->get(plugin) == 0,
        "CLAP reactivation failed to refresh the tail without a callback");
  plugin->deactivate(plugin);
  const auto *config = static_cast<const clap_plugin_audio_ports_config_t *>(
      plugin->get_extension(plugin, CLAP_EXT_AUDIO_PORTS_CONFIG));
  const auto *ports = static_cast<const clap_plugin_audio_ports_t *>(
      plugin->get_extension(plugin, CLAP_EXT_AUDIO_PORTS));
  const auto *configInfo = static_cast<const clap_plugin_audio_ports_config_info_t *>(
      plugin->get_extension(plugin, CLAP_EXT_AUDIO_PORTS_CONFIG_INFO));
  clap_audio_port_info_t preview{};
  clap_audio_port_info_t selected{};
  check(configInfo && configInfo->current_config(plugin) == 0 && ports &&
            configInfo->get(plugin, 1, 0, false, &preview) && preview.channel_count == 6 &&
            preview.id == 1 && preview.in_place_pair == 0 &&
            (preview.flags & CLAP_AUDIO_PORT_IS_MAIN) != 0 &&
            ports->get(plugin, 0, false, &selected) && selected.channel_count == 2 &&
            configInfo->current_config(plugin) == 0,
        "CLAP port preview returned incorrect metadata or changed the active selection");
  check(!configInfo->get(plugin, CLAP_INVALID_ID, 0, false, &preview) &&
            !configInfo->get(plugin, 0, 1, false, &preview) &&
            !configInfo->get(plugin, 0, 0, false, nullptr),
        "CLAP port preview accepted an invalid request");
  clap_audio_ports_config_t surround{};
  check(config && ports && config->count(plugin) == 5 &&
            config->get(plugin, 1, &surround) &&
            surround.main_input_channel_count == 6 && surround.main_output_channel_count == 6 &&
            surround.main_input_port_type && surround.main_output_port_type &&
            std::strcmp(surround.main_input_port_type, CLAP_PORT_SURROUND) == 0 &&
            std::strcmp(surround.main_output_port_type, CLAP_PORT_SURROUND) == 0 &&
            config->select(plugin, surround.id) && configInfo->current_config(plugin) == surround.id,
        "CLAP did not expose the declared six-channel configuration");
  clap_audio_port_info_t port{};
  check(ports->get(plugin, 0, false, &port) && port.channel_count == 6 && port.port_type &&
            std::strcmp(port.port_type, CLAP_PORT_SURROUND) == 0,
        "CLAP advertised the wrong selected output port");
  const auto *mapping = static_cast<const clap_plugin_surround_t *>(
      plugin->get_extension(plugin, CLAP_EXT_SURROUND));
  const std::array<std::uint8_t, 6> expected{
      CLAP_SURROUND_FL, CLAP_SURROUND_FR, CLAP_SURROUND_FC,
      CLAP_SURROUND_LFE, CLAP_SURROUND_BL, CLAP_SURROUND_BR};
  std::uint64_t mask{};
  for (const auto speaker : expected)
    mask |= std::uint64_t{1} << speaker;
  std::array<std::uint8_t, 6> map{};
  check(mapping && mapping->is_channel_mask_supported(plugin, mask) &&
            !mapping->is_channel_mask_supported(plugin, mask ^ 1),
        "CLAP surround mask support did not match the named layout");
  for (bool inputPort : {false, true}) {
    map.fill(0xff);
    check(mapping->get_channel_map(plugin, inputPort, 0, map.data(), 5) == 0 &&
              map.front() == 0xff &&
              mapping->get_channel_map(plugin, inputPort, 0, map.data(), 6) == 6 &&
              map == expected,
          "CLAP surround map was incorrect or ignored buffer capacity");
  }
  check(plugin->activate(plugin, 48000, 1, 64) && plugin->start_processing(plugin) &&
            !config->select(plugin, 0), "CLAP six-channel lifecycle failed");
  std::array<std::array<float, 65>, 6> wideInput{}, wideOutput{};
  std::array<float *, 6> wideIns{}, wideOuts{};
  for (unsigned c = 0; c < 6; ++c) {
    wideInput[c].fill(float(c));
    wideIns[c] = wideInput[c].data();
    wideOuts[c] = wideOutput[c].data();
  }
  in.data32 = wideIns.data();
  out.data32 = wideOuts.data();
  in.channel_count = out.channel_count = 6;
  process.frames_count = 65;
  check(plugin->process(plugin, &process) == CLAP_PROCESS_ERROR && wideOutput[5].back() == 0,
        "CLAP accepted a block larger than the activated capacity or copied the extra frame");
  process.frames_count = 64;
  wideOuts[5] = nullptr;
  check(plugin->process(plugin, &process) == CLAP_PROCESS_ERROR && wideOutput[4].front() == 0,
        "CLAP wrote to an output before checking all channel pointers");
  wideOutput[4].fill(-1);
  for (unsigned offset : {0u, 1u}) {
    wideOuts[5] = wideOuts[4] + offset;
    check(plugin->process(plugin, &process) == CLAP_PROCESS_ERROR &&
              std::all_of(wideOutput[4].begin(), wideOutput[4].end(),
                          [](float value) { return value == -1; }),
          "CLAP accepted overlapping outputs or wrote to them before rejection");
  }
  wideOuts[5] = wideOutput[5].data();
  check(plugin->process(plugin, &process) != CLAP_PROCESS_ERROR &&
            std::equal(wideOutput[5].begin(), wideOutput[5].begin() + 64, wideInput[5].begin()) &&
            wideOutput[5].back() == 0,
        "CLAP did not render the sixth channel");
  // Reuse the input buffers for a three-channel cycle.
  wideOuts[3] = wideIns[4];
  wideOuts[4] = wideIns[5];
  wideOuts[5] = wideIns[3];
  check(plugin->process(plugin, &process) != CLAP_PROCESS_ERROR, "CLAP rejected reused channel buffers");
  for (unsigned c = 3; c < 6; ++c)
    check(std::all_of(wideOuts[c], wideOuts[c] + 64, [c](float value) { return value == float(c); }),
          "CLAP overwrote a channel before reading its input");
  for (unsigned n = 0; n < 65; ++n) {
    wideInput[4][n] = float(n + 100);
    wideInput[5][n] = float(n + 200);
  }
  wideOuts[3] = wideOutput[3].data();
  wideOuts[4] = wideIns[5] + 1;
  wideOuts[5] = wideIns[4] + 1;
  check(plugin->process(plugin, &process) != CLAP_PROCESS_ERROR,
        "CLAP rejected partially overlapping main channel buffers");
  for (unsigned n = 0; n < 64; ++n)
    check(wideOuts[4][n] == float(n + 100) && wideOuts[5][n] == float(n + 200),
          "CLAP lost input samples during a shifted channel swap");
  std::array<float,65> keyLeft{}, keyRight{};
  keyLeft.fill(10);
  keyRight.fill(20);
  std::array<float*,2> key{keyLeft.data(),keyRight.data()};
  std::array<clap_audio_buffer_t,2> buses{in,{}};
  buses[1].data32=key.data();
  buses[1].channel_count=2;
  process.audio_inputs=buses.data();
  process.audio_inputs_count=2;
  for (unsigned c = 0; c < 6; ++c) {
    wideInput[c].fill(float(c));
    wideOuts[c] = wideOutput[c].data();
  }
  // The destination overlaps the first key channel by 63 samples.
  wideOuts[5]=keyLeft.data()+1;
  check(plugin->process(plugin,&process)!=CLAP_PROCESS_ERROR &&
            keyLeft.front()==10 &&
            std::all_of(keyLeft.begin()+1,keyLeft.end(),[](float sample){return sample==35;}) &&
            std::all_of(keyRight.begin(),keyRight.end(),[](float sample){return sample==20;}),
        "CLAP output overwrote sidechain samples before the processor could read them");
#if CELL_HAS_SSE_CONTROL
  // Both key samples are subnormal. Their sum would be normal without DAZ,
  // so output-only flushing cannot make this test pass.
  constexpr auto subnormal = std::bit_cast<float>(std::uint32_t{0x00400000});
  keyLeft.fill(subnormal);
  keyRight.fill(subnormal);
  wideInput[5].fill(0);
  wideOuts[5] = wideOutput[5].data();
  struct RestoreMxcsr {
    unsigned saved = _mm_getcsr();
    ~RestoreMxcsr() { _mm_setcsr(saved); }
  };
  {
    const RestoreMxcsr restore;
    constexpr unsigned denormalFlags = 0x8040u;
    const auto hostFlags = restore.saved & ~denormalFlags;
    _mm_setcsr(hostFlags);
    const auto status = plugin->process(plugin, &process);
    const auto flagsAfter = _mm_getcsr();
    check(status != CLAP_PROCESS_ERROR, "CLAP rejected a subnormal input block");
    check(flagsAfter == hostFlags, "CLAP changed host MXCSR settings");
  }
  check(std::all_of(wideOuts[5], wideOuts[5] + 64, [](float sample) {
          return std::bit_cast<std::uint32_t>(sample) == 0;
        }), "CLAP processor did not flush subnormal sidechain inputs");
  check(std::bit_cast<std::uint32_t>(keyLeft.front()) == 0x00400000u &&
            std::bit_cast<std::uint32_t>(keyRight.front()) == 0x00400000u,
        "CLAP modified out-of-place sidechain inputs");
#endif
  plugin->stop_processing(plugin);
  plugin->deactivate(plugin);
  const std::array<std::uint8_t, 8> bed{
      CLAP_SURROUND_FL, CLAP_SURROUND_FR, CLAP_SURROUND_FC, CLAP_SURROUND_LFE,
      CLAP_SURROUND_BL, CLAP_SURROUND_BR, CLAP_SURROUND_SL, CLAP_SURROUND_SR};
  std::array<std::uint8_t, 16> heightMap{};
  check(config->select(plugin, 2) &&
            mapping->get_channel_map(plugin, false, 0, heightMap.data(), heightMap.size()) == 10 &&
            std::equal(bed.begin(), bed.end(), heightMap.begin()) &&
            heightMap[8] == CLAP_SURROUND_TSL && heightMap[9] == CLAP_SURROUND_TSR,
        "CLAP 7.1.2 speaker order was incorrect");
  check(config->select(plugin, 3) &&
            mapping->get_channel_map(plugin, false, 0, heightMap.data(), heightMap.size()) == 12 &&
            std::equal(bed.begin(), bed.end(), heightMap.begin()) &&
            heightMap[8] == CLAP_SURROUND_TFL && heightMap[9] == CLAP_SURROUND_TFR &&
            heightMap[10] == CLAP_SURROUND_TBL && heightMap[11] == CLAP_SURROUND_TBR,
        "CLAP 7.1.4 speaker order was incorrect");
  check(config->select(plugin, 4) && ports->get(plugin, 0, false, &port) &&
            port.channel_count == 16 && !port.port_type &&
            mapping->get_channel_map(plugin, false, 0, heightMap.data(), heightMap.size()) == 0,
        "CLAP 9.1.6 should retain width without a speaker map");
  const auto *configurable = static_cast<const clap_plugin_configurable_audio_ports_t *>(
      plugin->get_extension(plugin, CLAP_EXT_CONFIGURABLE_AUDIO_PORTS));
  std::array<clap_audio_port_configuration_request_t, 2> requests{{
      {true, 0, 6, CLAP_PORT_SURROUND, expected.data()},
      {false, 0, 6, CLAP_PORT_SURROUND, expected.data()}}};
  check(configurable && configurable->can_apply_configuration(plugin, requests.data(), 2) &&
            configInfo->current_config(plugin) == 4 &&
            !configurable->apply_configuration(plugin, requests.data(), 1) &&
            configInfo->current_config(plugin) == 4,
        "CLAP layout preflight changed the selection or accepted a partial change");
  auto wrongOrder = expected;
  std::swap(wrongOrder[0], wrongOrder[1]);
  requests[1].port_details = wrongOrder.data();
  check(!configurable->apply_configuration(plugin, requests.data(), 2) &&
            configInfo->current_config(plugin) == 4,
        "CLAP accepted a different speaker order or committed part of a rejected request");
  requests[1] = requests[0];
  check(!configurable->can_apply_configuration(plugin, requests.data(), 2),
        "CLAP accepted duplicate port requests");
  requests[1].is_input = false;
  check(configurable->apply_configuration(plugin, requests.data(), 2) &&
            configInfo->current_config(plugin) == 1 &&
            plugin->activate(plugin, 48000, 1, 64),
        "CLAP failed to apply the requested 5.1 layout");
  check(!configurable->can_apply_configuration(plugin, requests.data(), 2) &&
            !configurable->apply_configuration(plugin, requests.data(), 2),
        "CLAP accepted a configuration request while active");
  plugin->deactivate(plugin);
  plugin->destroy(plugin);
  entry->deinit();
#if defined(_WIN32)
  FreeLibrary(library);
#else
  dlclose(library);
#endif
}
#else
void run(const char *path) {
  using namespace Steinberg;
  using namespace Steinberg::Vst;
  std::string error;
  auto module = VST3::Hosting::Module::create(path, error);
  if (!module)
    throw std::runtime_error(error);
  auto host = owned(new HostApplication);
  auto *context = static_cast<IHostApplication *>(host.get());
  const auto &factory = module->getFactory();
  factory.setHostContext(context);
  PluginContextFactory::instance().setPluginContext(context);
  struct Cleanup {
    const VST3::Hosting::PluginFactory &factory;
    ~Cleanup() {
      factory.setHostContext(nullptr);
      PluginContextFactory::instance().setPluginContext(nullptr);
    }
  } cleanup{factory};
  unsigned count = 0;
  for (const auto &info : factory.classInfos())
    if (info.category() == kVstAudioEffectClass) {
      ++count;
      PlugProvider provider(factory, info, true);
      check(provider.initialize(), "Transport VST3 init failed");
      auto component = provider.getComponentPtr();
      FUnknownPtr<IAudioProcessor> processor(component);
      check(processor != nullptr, "Transport VST3 processor missing");
      SpeakerArrangement stereo = SpeakerArr::kStereo;
      check(processor->setBusArrangements(&stereo, 1, &stereo, 1) == kResultOk,
            "Transport VST3 buses failed");
      component->activateBus(kAudio, kInput, 0, true);
      component->activateBus(kAudio, kOutput, 0, true);
      check(component->activateBus(kEvent, kInput, 0, true) == kResultOk &&
            component->activateBus(kEvent, kOutput, 0, true) == kResultOk,
            "VST3 event buses could not activate");
      ProcessSetup setup{kRealtime, kSample32, 64, 48000};
      check(processor->setupProcessing(setup) == kResultOk &&
                component->setActive(true) == kResultOk &&
                processor->setProcessing(true) == kResultOk,
            "Transport VST3 lifecycle failed");
      Channels input{}, values{};
      std::array<float *, 2> ins{input[0].data(), input[1].data()},
          outs{values[0].data(), values[1].data()};
      AudioBusBuffers in{}, out{};
      in.numChannels = out.numChannels = 2;
      in.channelBuffers32 = ins.data();
      out.channelBuffers32 = outs.data();
      ParameterChanges changes(1);
      int32 index{}, point{};
      ParamID id = 17;
      auto *queue = changes.addParameterData(id, index);
      check(queue && queue->addPoint(17, .5, point) == kResultOk,
            "Transport VST3 split event failed");
      ProcessData data{};
      data.processMode = kRealtime;
      data.symbolicSampleSize = kSample32;
      data.numSamples = 64;
      data.numInputs = data.numOutputs = 1;
      data.inputs = &in;
      data.outputs = &out;
      data.inputParameterChanges = &changes;
      for (bool flag : {false, true})
        for (double bpm : bpms) {
          ProcessContext transport{};
          transport.sampleRate = 48000;
          transport.state = flag ? ProcessContext::kTempoValid : 0;
          transport.tempo = bpm;
          data.processContext = &transport;
          check(processor->process(data) == kResultOk,
                "Transport VST3 render failed");
          output(values, flag && std::isfinite(bpm) && bpm > 0);
        }
      ProcessContext beat{};
      beat.sampleRate = 48000;
      beat.tempo = 137.5;
      beat.state = ProcessContext::kTempoValid;
      beat.projectTimeMusic = -.25;
      data.processContext = &beat;
      check(processor->process(data) == kResultOk && values[0][0] == 137.5f,
            "VST3 used a beat position without a valid host timeline");
      beat.state |= ProcessContext::kProjectTimeMusicValid;
      check(processor->process(data) == kResultOk && values[0][0] == -.25f,
            "VST3 did not pass a valid negative beat position");
      data.processContext = nullptr;
      check(processor->process(data) == kResultOk,
            "Transport VST3 missing context render failed");
      output(values, false);
      EventList eventInputs(3), eventOutputs(4);
      auto bytes = sysExPayload;
      Event sysex{};
      sysex.type = Event::kDataEvent;
      sysex.sampleOffset = 23;
      sysex.data = {uint32(bytes.size()), DataEvent::kMidiSysEx, bytes.data()};
      Event note{};
      note.type = Event::kNoteOnEvent;
      note.sampleOffset = 31;
      note.noteOn.pitch = 60;
      note.noteOn.velocity = 1.f;
      const std::array<std::uint8_t, 3> malformed{0xf0, 0x80, 0xf7};
      Event invalid = sysex;
      invalid.sampleOffset = 40;
      invalid.data = {uint32(malformed.size()), DataEvent::kMidiSysEx, malformed.data()};
      check(eventInputs.addEvent(sysex) == kResultOk && eventInputs.addEvent(note) == kResultOk &&
            eventInputs.addEvent(invalid) == kResultOk, "Could not stage the VST3 SysEx fixture");
      data.inputEvents = &eventInputs;
      data.outputEvents = &eventOutputs;
      check(processor->process(data) == kResultOk && eventOutputs.getEventCount() == 2,
            "VST3 dropped a valid event or returned malformed SysEx");
      bytes.fill(0);
      Event copied{}, forwardedNote{};
      check(eventOutputs.getEvent(0, copied) == kResultOk && copied.type == Event::kDataEvent &&
            copied.sampleOffset == 23 && copied.data.type == DataEvent::kMidiSysEx &&
            copied.data.size == sysExPayload.size() && copied.data.bytes &&
            std::equal(sysExPayload.begin(), sysExPayload.end(), copied.data.bytes),
            "VST3 lost SysEx data ownership or the sample offset");
      check(eventOutputs.getEvent(1, forwardedNote) == kResultOk &&
            forwardedNote.type == Event::kNoteOnEvent && forwardedNote.sampleOffset == 31,
            "VST3 reordered SysEx and channel MIDI");
      processor->setProcessing(false);
      component->setActive(false);
    }
  check(count == 1, "Transport VST3 factory did not expose one component");
}
#endif
} // namespace
int main(int argc, char **argv) {
  try {
    check(argc == 2, "Supply the transport fixture plugin path");
    run(argv[1]);
    std::cout << "PASS: real adapter valid/missing/NaN/infinite/nonpositive "
                 "tempo and parameter segmentation\n";
  } catch (const std::exception &e) {
    std::cerr << e.what() << '\n';
    return 1;
  }
}
