// Original Cell Framework implementation; see LICENSE.
#include "clap_trace.hpp"
#include <cell/dynamic_library.hpp>
#include <cell/files.hpp>
#include <clap/ext/audio-ports-config.h>
#include <clap/ext/preset-load.h>
#include <clap/ext/thread-check.h>
#include <algorithm>
#include <array>
#include <atomic>
#include <utility>
#include <chrono>
#include <cmath>
#include <condition_variable>
#include <filesystem>
#include <fstream>
#include <functional>
#include <iomanip>
#include <iostream>
#include <mutex>
#include <thread>
#include <variant>
#include <vector>

namespace {
using namespace cell::clap_trace;
using Clock = std::chrono::steady_clock;
using Nanoseconds = std::chrono::duration<double, std::nano>;

void require(bool ok, const char* message) {
    if (!ok) throw std::runtime_error(message);
}

bool flag(Reader& in) {
    const auto value = in.get<std::uint8_t>();
    require(value <= 1, "Invalid flag in CLAP trace");
    return value != 0;
}

using Event = std::variant<clap_event_param_value_t, clap_event_param_mod_t, clap_event_param_gesture_t,
                           clap_event_midi_t, clap_event_midi2_t, clap_event_midi_sysex_t,
                           clap_event_note_t, clap_event_note_expression_t, clap_event_transport_t>;

class InputEvents {
public:
    void decode(Reader& in, std::uint32_t frames) {
        const auto count = in.get<std::uint32_t>();
        require(count <= maxEvents, "Too many events in CLAP trace");
        items_.clear();
        items_.reserve(count);
        for (std::uint32_t index = 0; index < count; ++index) {
            clap_event_header_t head{};
            head.space_id = CLAP_CORE_EVENT_SPACE_ID;
            in(head.type, head.time, head.flags);
            require(head.time <= frames, "Event offset exceeds its block");
            switch (head.type) {
            case CLAP_EVENT_PARAM_VALUE:
                parameterFields(in, append<clap_event_param_value_t>(head)); break;
            case CLAP_EVENT_PARAM_MOD:
                parameterFields(in, append<clap_event_param_mod_t>(head)); break;
            case CLAP_EVENT_PARAM_GESTURE_BEGIN: case CLAP_EVENT_PARAM_GESTURE_END:
                in(append<clap_event_param_gesture_t>(head).param_id); break;
            case CLAP_EVENT_MIDI: {
                auto& event = append<clap_event_midi_t>(head);
                in(event.port_index, event.data);
                break;
            }
            case CLAP_EVENT_MIDI2: {
                auto& event = append<clap_event_midi2_t>(head);
                in(event.port_index, event.data);
                break;
            }
            case CLAP_EVENT_MIDI_SYSEX: {
                auto& event = append<clap_event_midi_sysex_t>(head);
                in(event.port_index);
                const auto bytes = in.blob();
                event.buffer = reinterpret_cast<const std::uint8_t*>(bytes.data());
                event.size = static_cast<std::uint32_t>(bytes.size());
                break;
            }
            case CLAP_EVENT_NOTE_ON: case CLAP_EVENT_NOTE_OFF: case CLAP_EVENT_NOTE_CHOKE: case CLAP_EVENT_NOTE_END: {
                auto& event = append<clap_event_note_t>(head);
                noteFields(in, event); in(event.velocity);
                break;
            }
            case CLAP_EVENT_NOTE_EXPRESSION: {
                auto& event = append<clap_event_note_expression_t>(head);
                in(event.expression_id); noteFields(in, event); in(event.value);
                break;
            }
            case CLAP_EVENT_TRANSPORT:
                transportFields(in, append<clap_event_transport_t>(head)); break;
            default: throw std::runtime_error("Unsupported event type in CLAP trace");
            }
        }
    }

    clap_input_events_t api{this,
        [](const clap_input_events_t* api) {
            return static_cast<std::uint32_t>(static_cast<const InputEvents*>(api->ctx)->items_.size());
        },
        [](const clap_input_events_t* api, std::uint32_t index) -> const clap_event_header_t* {
            const auto& items = static_cast<const InputEvents*>(api->ctx)->items_;
            if (index >= items.size()) return nullptr;
            return std::visit([](const auto& item) { return &item.header; }, items[index]);
        }};
private:
    template<class T> T& append(clap_event_header_t header) {
        header.size = sizeof(T);
        auto& item = items_.emplace_back(std::in_place_type<T>);
        auto& event = std::get<T>(item);
        event.header = header;
        return event;
    }
    std::vector<Event> items_;
};

class AudioThread {
public:
    AudioThread() : worker_([this] { loop(); }) {}
    ~AudioThread() {
        {
            const std::scoped_lock lock(mutex_);
            done_ = true;
        }
        ready_.notify_one();
        worker_.join();
    }
    bool isCurrent() const noexcept { return worker_.get_id() == std::this_thread::get_id(); }
    template<class Callable> void run(Callable&& call) {
        std::unique_lock lock(mutex_);
        job_ = [&call] { call(); };
        ready_.notify_one();
        finished_.wait(lock, [&] { return !job_; });
    }
private:
    void loop() {
        std::unique_lock lock(mutex_);
        for (;;) {
            ready_.wait(lock, [&] { return job_ || done_; });
            if (done_) return;
            lock.unlock();
            job_();
            lock.lock();
            job_ = nullptr;
            finished_.notify_one();
        }
    }
    bool done_{};
    std::mutex mutex_;
    std::condition_variable ready_, finished_;
    std::function<void()> job_;
    std::thread worker_;
};

struct Bus {
    void resize(std::uint32_t channels, std::uint32_t frames) {
        require(channels <= 64, "Too many audio channels in CLAP trace");
        samples.resize(std::size_t(channels) * std::max(frames, 1u));
        pointers.resize(channels);
        const auto stride = std::max(frames, 1u);
        for (std::uint32_t index = 0; index < channels; ++index)
            pointers[index] = samples.data() + std::size_t(index) * stride;
    }
    std::vector<float> samples;
    std::vector<float*> pointers;
};

class Profile {
public:
    Profile(bool enabled, std::size_t traceBytes) : enabled_(enabled) {
        if (enabled_) samples_.reserve(traceBytes / 32);
    }
    void resetSchedule() { next_ = {}; }
    clap_process_status process(const clap_plugin_t* plugin, const clap_process_t& process,
                                double rate, bool offline, const AllocationProbe* probe) {
        if (!enabled_) return plugin->process(plugin, &process);
        const bool paced = !offline && process.frames_count;
        const Nanoseconds budget(1e9 * process.frames_count / rate);
        if (paced) {
            if (next_ == Clock::time_point{}) next_ = Clock::now();
            else std::this_thread::sleep_until(next_);
        } else resetSchedule();
        const auto start = Clock::now();
        const auto status = plugin->process(plugin, &process);
        const auto end = Clock::now();
        const auto elapsed = Nanoseconds(end - start);
        if (probe) allocations_ += probe->count(plugin);
        if (elapsed > budget && process.frames_count) ++misses_;
        if (paced) {
            const auto endOfPeriod = next_ + std::chrono::duration_cast<Clock::duration>(budget);
            if (end > endOfPeriod || status == CLAP_PROCESS_ERROR) {
                ++underruns_;
                next_ = end;
            } else next_ = endOfPeriod;
            ++pacedCallbacks_;
        }
        samples_.push_back(elapsed.count());
        return status;
    }
    void write(std::ostream& out, bool allocationProbe) {
        if (!enabled_) return;
        std::ranges::sort(samples_);
        const auto p999 = (samples_.size() * 999 + 999) / 1000;
        out << ",\n  \"realtime\": {\n"
            << "    \"measurement\": \"clap-process-wall-clock\",\n"
            << "    \"callback_count\": " << samples_.size() << ",\n"
            << "    \"paced_callback_count\": " << pacedCallbacks_ << ",\n"
            << "    \"p99_9_callback_ns\": ";
        if (samples_.empty()) out << "null"; else out << samples_[p999 - 1];
        out << ",\n    \"worst_callback_ns\": ";
        if (samples_.empty()) out << "null"; else out << samples_.back();
        out << ",\n    \"deadline_misses\": " << misses_
            << ",\n    \"allocation_scope\": "
            << (allocationProbe ? "\"plugin-cpp-new-on-callback-thread\"" : "null")
            << ",\n    \"allocations\": ";
        if (allocationProbe) out << allocations_; else out << "null";
        out << ",\n    \"underruns\": ";
        if (pacedCallbacks_) out << underruns_; else out << "null";
        out << ",\n    \"underrun_source\": \"simulated-playback-scheduler\""
            << ",\n    \"device_underruns\": null\n  }";
    }
private:
    bool enabled_;
    std::vector<double> samples_;
    Clock::time_point next_{};
    std::uint64_t pacedCallbacks_{}, misses_{}, underruns_{}, allocations_{};
};

struct Replay {
    Replay(bool profile, std::size_t traceBytes) : profile(profile, traceBytes), profiling(profile) {}
    ~Replay() {
        if (plugin) {
            if (processing) audio.run([&] { plugin->stop_processing(plugin); });
            if (active) plugin->deactivate(plugin);
            plugin->destroy(plugin);
        }
        if (entryInitialized) entry->deinit();
    }

    template<class Extension> const Extension* extension(const char* id) const {
        return static_cast<const Extension*>(plugin->get_extension(plugin, id));
    }

    void open(std::string_view module, const char* bundle, std::string_view id) {
        require(library.open(cell::Text(module)), "Could not open the CLAP module");
        entry = static_cast<const clap_plugin_entry_t*>(library.getFunction("clap_entry"));
        require(entry && clap_version_is_compatible(entry->clap_version) && entry->init(bundle),
                "Could not initialize CLAP entry");
        entryInitialized = true;
        const auto* factory = static_cast<const clap_plugin_factory_t*>(entry->get_factory(CLAP_PLUGIN_FACTORY_ID));
        require(factory != nullptr, "Missing CLAP factory");
        for (std::uint32_t index = 0; index < factory->get_plugin_count(factory); ++index) {
            const auto* descriptor = factory->get_plugin_descriptor(factory, index);
            if (descriptor && descriptor->id && id == descriptor->id) {
                plugin = factory->create_plugin(factory, &host, descriptor->id);
                break;
            }
        }
        require(plugin != nullptr, "Trace plug-in ID was not found in the CLAP bundle");
        require(plugin->init(plugin), "Could not initialize the CLAP plug-in");
        if (profiling) {
            probe = extension<AllocationProbe>(allocationExtension);
            if (probe) probe->enable(plugin, true);
        }
    }

    void replay(Reader& in);
    void step(Op op, Reader& in);
    void block(Reader& in);

    cell::DynamicLibrary library;
    const clap_plugin_entry_t* entry{};
    const clap_plugin_t* plugin{};
    bool entryInitialized{}, active{}, processing{}, offline{};
    std::uint32_t maxBlock{}, minBlock{}, channels{};
    std::uint64_t callbacks{}, flushes{}, stateLoads{}, stateSaves{}, transportBlocks{}, inputEvents{}, outputEvents{}, editorEdits{}, processErrors{};
    double rate{};
    struct Configuration {
        double rate;
        std::uint32_t minFrames, maxFrames, channels;
    };
    std::vector<Configuration> configurations;
    AudioThread audio;
    const std::thread::id mainThread = std::this_thread::get_id();
    std::atomic<bool> mainCallback{};
    Profile profile;
    bool profiling;
    const AllocationProbe* probe{};
    InputEvents events;
    Bus output;
    std::array<Bus, 2> inputs;
    clap_output_events_t outgoing{this,
        [](const clap_output_events_t* events, const clap_event_header_t*) {
            ++static_cast<Replay*>(events->ctx)->outputEvents;
            return true;
        }};
    const clap_host_thread_check_t threadCheck{
        [](const clap_host_t* host) {
            return static_cast<const Replay*>(host->host_data)->mainThread == std::this_thread::get_id();
        },
        [](const clap_host_t* host) { return static_cast<const Replay*>(host->host_data)->audio.isCurrent(); }};
    const clap_host_t host{CLAP_VERSION, this, "Cell Doctor", "Cell", "", "1",
        [](const clap_host_t* host, const char* id) -> const void* {
            if (std::strcmp(id, CLAP_EXT_THREAD_CHECK) == 0)
                return &static_cast<const Replay*>(host->host_data)->threadCheck;
            return nullptr;
        },
        [](const clap_host_t*) {}, [](const clap_host_t*) {},
        [](const clap_host_t* host) { static_cast<Replay*>(host->host_data)->mainCallback = true; }};
};

void Replay::block(Reader& in) {
    require(processing, "Process occurred outside the CLAP processing lifecycle");
    clap_process_t process{};
    in(process.frames_count, process.steady_time);
    require(process.frames_count <= maxBlock, "Block exceeds its CLAP activation limit");
    const bool transportPresent = flag(in);
    const bool copyAudio = flag(in);
    clap_event_transport_t transport{};
    if (transportPresent) {
        transport.header = {sizeof(transport), 0, CLAP_CORE_EVENT_SPACE_ID, CLAP_EVENT_TRANSPORT, 0};
        transportFields(in, transport);
        process.transport = &transport;
        ++transportBlocks;
    }

    std::uint32_t outputWidth{}, inputCount{};
    in(outputWidth, inputCount);
    require(channels == outputWidth && inputCount <= inputs.size(), "Invalid CLAP audio layout in trace");
    std::array<clap_audio_buffer_t, 2> inputPorts{};
    for (std::uint32_t index = 0; index < inputCount; ++index) {
        auto& input = inputs[index];
        auto& port = inputPorts[index];
        in(port.channel_count, port.constant_mask, port.latency);
        input.resize(port.channel_count, process.frames_count);
        for (auto& channel : input.pointers) {
            if (!flag(in)) channel = nullptr;
            else if (copyAudio) {
                const auto samples = in.raw(sizeof(float) * process.frames_count);
                std::memcpy(channel, samples.data(), samples.size());
            } else std::fill_n(channel, process.frames_count, 0.f);
        }
        if (port.channel_count) port.data32 = input.pointers.data();
    }
    output.resize(outputWidth, process.frames_count);
    clap_audio_buffer_t outputPort{output.pointers.data(), nullptr, outputWidth};
    events.decode(in, process.frames_count);
    inputEvents += events.api.size(&events.api);
    process.audio_inputs = inputPorts.data();
    process.audio_inputs_count = inputCount;
    process.audio_outputs = &outputPort;
    process.audio_outputs_count = 1;
    process.in_events = &events.api;
    process.out_events = &outgoing;
    require(in.get<Op>() == Op::result, "Missing recorded CLAP process result");
    const auto expected = in.get<clap_process_status>();
    clap_process_status status{};
    audio.run([&] { status = profile.process(plugin, process, rate, offline, probe); });
    ++callbacks;
    if (status == CLAP_PROCESS_ERROR) ++processErrors;
    require(status == expected, "CLAP process status differed from the original callback");
}

void Replay::step(Op op, Reader& in) {
    switch (op) {
    case Op::activate: {
        require(!active, "CLAP plug-in activated twice");
        in(rate, minBlock, maxBlock, channels);
        require(std::isfinite(rate) && rate >= 1000 && rate <= 768000 &&
                minBlock > 0 && minBlock <= maxBlock && maxBlock <= maxFrames &&
                channels > 0 && channels <= 64, "Invalid CLAP activation in trace");
        if (const auto* configs = extension<clap_plugin_audio_ports_config_t>(CLAP_EXT_AUDIO_PORTS_CONFIG)) {
            bool configured = false;
            for (std::uint32_t index = 0; index < configs->count(plugin); ++index) {
                clap_audio_ports_config_t info{};
                if (configs->get(plugin, index, &info) && info.main_output_channel_count == channels) {
                    configured = configs->select(plugin, info.id);
                    break;
                }
            }
            require(configured, "Could not select recorded CLAP port configuration");
        }
        require(plugin->activate(plugin, rate, minBlock, maxBlock), "CLAP activation failed");
        active = true;
        configurations.push_back({rate, minBlock, maxBlock, channels});
        break;
    }
    case Op::deactivate:
        require(active && !processing, "Invalid CLAP deactivation");
        plugin->deactivate(plugin);
        active = false;
        break;
    case Op::start:
        require(active && !processing, "Invalid CLAP start-processing call");
        audio.run([&] { processing = plugin->start_processing(plugin); });
        require(processing, "Could not start CLAP audio processing");
        profile.resetSchedule();
        break;
    case Op::stop:
        require(processing, "Invalid CLAP stop-processing call");
        audio.run([&] { plugin->stop_processing(plugin); });
        processing = false;
        break;
    case Op::reset:
        require(active, "CLAP reset outside activation");
        audio.run([&] { plugin->reset(plugin); });
        profile.resetSchedule();
        break;
    case Op::process:
        block(in);
        break;
    case Op::flush: {
        const auto* params = extension<clap_plugin_params_t>(CLAP_EXT_PARAMS);
        require(params != nullptr, "CLAP target has no parameter interface");
        events.decode(in, maxFrames);
        inputEvents += events.api.size(&events.api);
        const auto flush = [&] { params->flush(plugin, &events.api, &outgoing); };
        if (active) audio.run(flush); else flush();
        ++flushes;
        break;
    }
    case Op::load: {
        const auto* state = extension<clap_plugin_state_t>(CLAP_EXT_STATE);
        require(state != nullptr, "CLAP target has no state interface");
        const bool expected = flag(in);
        auto bytes = in.blob();
        const clap_istream_t stream{&bytes, [](const clap_istream_t* stream, void* dest, std::uint64_t size) {
            auto& source = *static_cast<std::span<const std::byte>*>(stream->ctx);
            const auto count = std::min<std::uint64_t>(source.size(), size);
            if (count) std::memcpy(dest, source.data(), count);
            source = source.subspan(static_cast<std::size_t>(count));
            return static_cast<std::int64_t>(count);
        }};
        require(state->load(plugin, &stream) == expected, "CLAP state load acceptance differed");
        ++stateLoads;
        break;
    }
    case Op::save: {
        const auto* state = extension<clap_plugin_state_t>(CLAP_EXT_STATE);
        require(state != nullptr, "CLAP target has no state interface");
        auto bytes = in.blob();
        const clap_ostream_t stream{&bytes, [](const clap_ostream_t* stream, const void* data, std::uint64_t size) -> std::int64_t {
            auto& expected = *static_cast<std::span<const std::byte>*>(stream->ctx);
            if (size > expected.size() || (size && std::memcmp(data, expected.data(), size))) return -1;
            expected = expected.subspan(static_cast<std::size_t>(size));
            return static_cast<std::int64_t>(size);
        }};
        require(state->save(plugin, &stream) && bytes.empty(), "CLAP state snapshot differed");
        ++stateSaves;
        break;
    }
    case Op::render: {
        const auto* render = extension<clap_plugin_render_t>(CLAP_EXT_RENDER);
        const auto mode = in.get<clap_plugin_render_mode>();
        require((mode == CLAP_RENDER_REALTIME || mode == CLAP_RENDER_OFFLINE) &&
                render && render->set(plugin, mode), "CLAP target rejected recorded render mode");
        offline = mode == CLAP_RENDER_OFFLINE;
        profile.resetSchedule();
        break;
    }
    case Op::preset: {
        const auto* presets = extension<clap_plugin_preset_load_t>(CLAP_EXT_PRESET_LOAD);
        const std::string key(in.string());
        require(presets && presets->from_location(plugin, CLAP_PRESET_DISCOVERY_LOCATION_PLUGIN, nullptr, key.c_str()),
                "CLAP target rejected recorded preset recall");
        break;
    }
    case Op::editor: {
        const auto* editor = extension<EditorProbe>(editorExtension);
        const auto count = in.get<std::uint32_t>();
        require(editor && count && count <= maxEvents, "Invalid editor batch or missing Cell editor replay interface");
        for (std::uint32_t index = 0; index < count; ++index) {
            clap_id parameter{};
            std::uint8_t type{};
            double value{};
            in(parameter, type, value);
            require(editor->edit(plugin, parameter, type, value), "CLAP target rejected a recorded editor edit");
        }
        editorEdits += count;
        break;
    }
    case Op::incomplete:
        throw std::runtime_error("Incomplete recording (capacity, unsupported input, or overlapping host callbacks)");
    default:
        throw std::runtime_error("Invalid command in CLAP trace");
    }
}

void Replay::replay(Reader& in) {
    for (std::size_t index = 1; ; ++index) {
        try {
            const auto op = in.get<Op>();
            if (op == Op::end) {
                require(in.empty() && !active && !processing, "Invalid end of CLAP recording");
                require(callbacks || flushes || stateLoads || stateSaves || editorEdits, "Recording contains no interactions");
                return;
            }
            step(op, in);
            if (mainCallback.exchange(false)) plugin->on_main_thread(plugin);
        } catch (const std::exception& error) {
            throw std::runtime_error("Trace command " + std::to_string(index) + ": " + error.what());
        }
    }
}

void verifySignature(Reader& in) {
    const auto magic = in.raw(signature.size());
    require(std::memcmp(magic.data(), signature.data(), signature.size()) == 0, "Unrecognized CLAP trace version");
}
} // namespace

int run(std::span<const std::string> args) try {
    require(args.size() == 5 || (args.size() == 6 && args[5] == "--realtime"),
            "Usage: cell-doctor-clap MODULE BUNDLE TRACE REPORT [--realtime]");
    const auto bytes = cell::files::readBounded(std::filesystem::path(reinterpret_cast<const char8_t*>(args[3].c_str())), maxBytes);
    Reader in(bytes);
    verifySignature(in);
    const auto id = in.string();
    require(!id.empty() && id.size() <= 1024 && id.find('\0') == id.npos, "Invalid CLAP trace plug-in ID");
    // Reject a known incomplete trace before loading any plug-in code.
    require(bytes.back() == static_cast<std::byte>(Op::end),
            "Incomplete CLAP recording (truncation, capacity, unsupported input, or overlapping host calls)");
    Replay replay(args.size() == 6, bytes.size());
    replay.open(args[1], args[2].c_str(), id);
    std::string failure;
    try { replay.replay(in); }
    catch (const std::exception& error) { failure = error.what(); }
    std::ofstream report(std::filesystem::path(reinterpret_cast<const char8_t*>(args[4].c_str())), std::ios::binary | std::ios::trunc);
    report << std::setprecision(12) << "{\n  \"schema\": \"cell.doctor.clap.v1\","
           << "\n  \"status\": \"" << (failure.empty() ? "passed" : "failed") << "\","
           << "\n  \"callbacks\": " << replay.callbacks
           << ",\n  \"process_errors\": " << replay.processErrors
           << ",\n  \"editor_edits\": " << replay.editorEdits
           << ",\n  \"input_events\": " << replay.inputEvents
           << ",\n  \"output_events\": " << replay.outputEvents
           << ",\n  \"transport_blocks\": " << replay.transportBlocks
           << ",\n  \"flushes\": " << replay.flushes
           << ",\n  \"state_loads\": " << replay.stateLoads
           << ",\n  \"state_saves\": " << replay.stateSaves;
    report << ",\n  \"activation_configurations\": [";
    const char* separator = "";
    for (const auto& config : replay.configurations) {
        report << separator << "\n    {\"sample_rate\": " << config.rate
               << ", \"min_frames\": " << config.minFrames << ", \"max_frames\": "
               << config.maxFrames << ", \"output_channels\": " << config.channels << '}';
        separator = ",";
    }
    if (!replay.configurations.empty()) report << "\n  ";
    report << ']';
    replay.profile.write(report, replay.probe != nullptr);
    report << "\n}\n";
    report.close();
    require(bool(report), "Could not write the CLAP replay report");
    if (!failure.empty()) throw std::runtime_error(failure);
    std::cout << "Completed " << replay.callbacks << " CLAP process callbacks\n";
    return 0;
} catch (const std::exception& error) {
    std::cerr << error.what() << '\n';
    return 1;
}

#if defined(_WIN32)
int wmain(int argc, wchar_t** argv) {
    std::vector<std::string> args;
    for (int index = 0; index < argc; ++index) {
        const auto utf8 = std::filesystem::path(argv[index]).u8string();
        args.emplace_back(reinterpret_cast<const char*>(utf8.data()), utf8.size());
    }
    return run(args);
}
#else
int main(int argc, char** argv) {
    const std::vector<std::string> args(argv, argv + argc);
    return run(args);
}
#endif
