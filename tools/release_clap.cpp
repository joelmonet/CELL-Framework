// Original Cell Framework implementation; see LICENSE.
#include "clap_session.hpp"
#include <cell/audio_file.hpp>
#include <clap/ext/preset-load.h>
#include <clap/factory/preset-discovery.h>
#include <array>
#include <cmath>
#include <iostream>
#include <set>

namespace {
using namespace cell;
using namespace cell::clap_session;
using V = json::Value;
using O = V::Object;
using A = V::Array;
constexpr unsigned defaultRate = 48000, defaultFrames = 4097, blockFrames = 128;
constexpr std::uint64_t maxSamples = 8 * 1024 * 1024;

std::uint32_t integer(const V& field, std::uint32_t maximum) {
    const auto* value = field.number();
    require(value && *value >= 0 && *value <= maximum && std::trunc(*value) == *value,
            "Invalid integer in the release fixture");
    return static_cast<std::uint32_t>(*value);
}

struct Layout {
    std::vector<unsigned> inputs;
    unsigned outputs{};
    V description;
};

Layout audioLayout(const Plugin& plug) {
    const auto* ports = plug.extension<clap_plugin_audio_ports_t>(CLAP_EXT_AUDIO_PORTS);
    const auto read = [&](bool input) {
        const auto count = ports->count(plug.instance, input);
        require(count <= 8, "Release host supports at most eight input buses");
        A result;
        for (unsigned index = 0; index < count; ++index) {
            clap_audio_port_info_t info{};
            require(ports->get(plug.instance, index, input, &info) && info.channel_count &&
                    info.channel_count <= 64, "Unsupported release audio port");
            result.emplace_back(O{{"id", V(info.id)}, {"channels", V(info.channel_count)}, {"flags", V(info.flags)}});
        }
        return result;
    };
    auto inputs = read(true);
    auto outputs = read(false);
    require(outputs.size() == 1, "Release host needs one output bus");
    Layout layout;
    for (const auto& port : inputs) layout.inputs.push_back(integer(port["channels"], 64));
    layout.outputs = integer(outputs.front()["channels"], 64);
    layout.description = V(O{{"inputs", V(std::move(inputs))}, {"outputs", V(std::move(outputs))}});
    return layout;
}

struct Audio {
    unsigned rate{}, frames{};
    std::vector<std::vector<float>> samples;
    std::vector<float*> pointers(unsigned offset = 0) {
        std::vector<float*> result;
        for (auto& channel : samples) result.push_back(channel.data() + offset);
        return result;
    }
};

Audio readAudio(const File& path) {
    const auto reader = audio::openAudioFile(path);
    const auto& info = reader->info();
    require(info.channels > 0 && info.channels <= 64 && info.frames > 0 &&
            static_cast<std::uint64_t>(info.frames) <= maxSamples / info.channels &&
            info.sampleRate >= 1000 && info.sampleRate <= 768000, "Release WAVE exceeds the supported audio bounds");
    Audio result{static_cast<unsigned>(info.sampleRate), static_cast<unsigned>(info.frames),
                 std::vector<std::vector<float>>(info.channels, std::vector<float>(info.frames))};
    auto planes = result.pointers();
    require(reader->read(planes.data(), info.channels, 0, result.frames), "Could not read the release WAVE asset");
    for (const auto& channel : result.samples)
        require(std::ranges::all_of(channel, [](float value) { return std::isfinite(value); }), "Release WAVE contains non-finite audio");
    return result;
}

void writeAudio(const File& path, const Audio& signal) {
    const auto count = static_cast<unsigned>(signal.samples.size());
    auto writer = audio::createWaveFile(path, {signal.rate, count, 32, true});
    std::vector<const float*> planes;
    for (const auto& channel : signal.samples) planes.push_back(channel.data());
    require(writer->write(planes.data(), count, signal.frames) && writer->finish().wasOk(),
            "Could not write the release reference WAVE");
}

Audio stimulus(unsigned channels) {
    Audio result{defaultRate, defaultFrames,
                 std::vector<std::vector<float>>(channels, std::vector<float>(defaultFrames))};
    for (unsigned channel = 0; channel < channels; ++channel)
        for (unsigned frame = 0; frame < result.frames; ++frame)
            result.samples[channel][frame] = float(int((frame + channel * 11) % (127 - channel)) - 63) / 128;
    return result;
}

struct Point {
    unsigned sample;
    clap_id parameter;
    double value;
};

std::vector<Point> automation(const V& config, const V& mapping, unsigned frames) {
    std::vector<Point> result;
    const auto admits = [](const V& parameter) {
        const auto* flags = parameter["flags"].number();
        return flags && (static_cast<unsigned>(*flags) & CLAP_PARAM_IS_AUTOMATABLE) &&
               !(static_cast<unsigned>(*flags) & CLAP_PARAM_IS_READONLY);
    };
    if (const auto* points = config.array()) {
        require(points->size() <= 4096, "Too many release automation points");
        for (const auto& event : *points) {
            const auto sample = integer(event["sample"], frames - 1);
            const auto id = integer(event["parameter"], CLAP_INVALID_ID - 1);
            const auto& parameter = mapping[std::to_string(id)];
            const auto* value = event["value"].number();
            require(admits(parameter) && value && *value >= *parameter["minimum"].number() &&
                    *value <= *parameter["maximum"].number() && (result.empty() || sample >= result.back().sample),
                    "Release automation refers to an unknown, read-only or unsorted lane");
            result.push_back({sample, id, *value});
        }
    } else {
        require(config.isNull(), "Release automation must be an array");
        require(frames > 275, "The default automation needs at least 276 input frames");
        for (const auto& [key, parameter] : *mapping.object()) {
            if (!admits(parameter)) continue;
            const auto minimum = *parameter["minimum"].number(), maximum = *parameter["maximum"].number();
            if (minimum == maximum) continue;
            const auto id = static_cast<clap_id>(std::stoul(key));
            result.push_back({19, id, maximum});
            result.push_back({275, id, minimum});
            break;
        }
    }
    require(!result.empty(), "Declare at least one released automation lane");
    return result;
}

V automationJson(const std::vector<Point>& points) {
    A result;
    for (const auto& point : points)
        result.emplace_back(O{{"sample", V(point.sample)}, {"parameter", V(point.parameter)}, {"value", V(point.value)}});
    return V(std::move(result));
}

V values(const Plugin& plugin, const V& mapping) {
    O result;
    for (const auto& [key, field] : *mapping.object())
        result.emplace(key, V(plugin.value(static_cast<clap_id>(std::stoul(key)))));
    return V(std::move(result));
}

bool instrument(const Plugin& plugin) {
    for (const char* const* feature = plugin.instance->desc->features; feature && *feature; ++feature)
        if (std::string_view(*feature) == CLAP_PLUGIN_FEATURE_INSTRUMENT) return true;
    return false;
}

struct Render {
    Audio signal;
    A checkpoints;
};

Render render(const Plugin& plug, const Layout& layout, Audio source, const std::vector<Point>& points) {
    const auto* instance = plug.instance;
    require(layout.inputs.empty() || source.samples.size() == layout.inputs.front(), "Input WAVE channel count differs from the released main bus");
    require(source.frames <= maxSamples / layout.outputs, "Output exceeds the release render size limit");
    Render result{{source.rate, source.frames,
        std::vector<std::vector<float>>(layout.outputs, std::vector<float>(source.frames))}, {}};
    require(plug.extension<clap_plugin_render_t>(CLAP_EXT_RENDER)->set(instance, CLAP_RENDER_OFFLINE),
            "Plug-in refused offline render mode");
    require(instance->activate(instance, source.rate, 1, blockFrames), "Could not activate the released audio layout");
    struct Lifetime {
        const clap_plugin_t* plugin;
        bool started{};
        ~Lifetime() {
            if (started) plugin->stop_processing(plugin);
            plugin->deactivate(plugin);
        }
    } lifetime{instance, instance->start_processing(instance)};
    require(lifetime.started, "Could not start offline processing");
    std::array<float, blockFrames> silence{};
    std::vector<std::vector<float*>> inputPointers(layout.inputs.size());
    std::vector<clap_audio_buffer_t> inputBuffers(layout.inputs.size());
    for (std::size_t bus = 0; bus < layout.inputs.size(); ++bus) {
        inputPointers[bus].assign(layout.inputs[bus], silence.data());
        inputBuffers[bus].data32 = inputPointers[bus].data();
        inputBuffers[bus].channel_count = layout.inputs[bus];
    }
    std::vector<float*> outputPointers(layout.outputs);
    clap_audio_buffer_t output{};
    output.data32 = outputPointers.data();
    output.channel_count = layout.outputs;
    std::size_t next{};
    const bool playNote = instrument(plug);
    for (unsigned start = 0; start < source.frames; start += blockFrames) {
        const auto count = std::min(blockFrames, source.frames - start);
        std::vector<clap_event_param_value_t> edits;
        std::map<clap_id, double> edited;
        while (next < points.size() && points[next].sample < start + count) {
            const auto& point = points[next++];
            edits.push_back(edit(point.parameter, point.sample - start, point.value));
            edited[point.parameter] = point.value;
        }
        std::vector<const clap_event_header_t*> events;
        for (const auto& edit : edits) events.push_back(&edit.header);
        std::array<clap_event_midi_t, 2> notes{};
        const unsigned release = std::max(1u, source.frames * 3 / 4);
        if (playNote) {
            const auto note = [&](unsigned index, unsigned offset, std::uint8_t type, std::uint8_t velocity) {
                auto& event = notes[index];
                event.header = {sizeof(event), offset, CLAP_CORE_EVENT_SPACE_ID, CLAP_EVENT_MIDI, 0};
                event.data[0] = type;
                event.data[1] = 60;
                event.data[2] = velocity;
                events.push_back(&event.header);
            };
            if (!start) note(0, 0, 0x90, 100);
            if (start <= release && release < start + count) note(1, release - start, 0x80, 0);
            std::stable_sort(events.begin(), events.end(), [](const auto* a, const auto* b) { return a->time < b->time; });
        }
        const clap_input_events_t incoming{&events, [](const clap_input_events_t* list) {
            return static_cast<std::uint32_t>(static_cast<const decltype(events)*>(list->ctx)->size());
        }, [](const clap_input_events_t* list, std::uint32_t i) {
            const auto& events = *static_cast<const std::vector<const clap_event_header_t*>*>(list->ctx);
            return i < events.size() ? events[i] : nullptr;
        }};
        for (unsigned channel = 0; !layout.inputs.empty() && channel < layout.inputs.front(); ++channel)
            inputPointers[0][channel] = source.samples[channel].data() + start;
        for (unsigned channel = 0; channel < layout.outputs; ++channel)
            outputPointers[channel] = result.signal.samples[channel].data() + start;
        const clap_output_events_t outgoing{nullptr, [](const clap_output_events_t*, const clap_event_header_t*) { return true; }};
        const clap_process_t process{start, count, nullptr, inputBuffers.data(), &output,
            static_cast<std::uint32_t>(inputBuffers.size()), 1, &incoming, &outgoing};
        require(instance->process(instance, &process) != CLAP_PROCESS_ERROR, "Offline CLAP processing failed");
        for (const auto& [id, expected] : edited) {
            const auto actual = plug.value(id);
            require(std::isfinite(actual) && std::abs(actual - expected) <= 1e-9 * std::max(1.0, std::abs(expected)),
                    "Released automation was not applied to its declared parameter");
            result.checkpoints.emplace_back(O{{"sample", V(start + count - 1)}, {"parameter", V(id)}, {"value", V(actual)}});
        }
    }
    require(next == points.size(), "Release automation was not delivered");
    return result;
}

V measure(const Audio& actual, const Audio& expected, const V& limits) {
    require(actual.rate == expected.rate && actual.frames == expected.frames &&
            actual.samples.size() == expected.samples.size(), "Offline render shape changed");
    double peak{}, sum{};
    for (std::size_t channel = 0; channel < actual.samples.size(); ++channel)
        for (unsigned frame = 0; frame < actual.frames; ++frame) {
            require(std::isfinite(actual.samples[channel][frame]), "Offline render contains non-finite audio");
            const auto error = std::abs(double(actual.samples[channel][frame]) - expected.samples[channel][frame]);
            peak = std::max(peak, error);
            sum += error * error;
        }
    const auto rms = std::sqrt(sum / (actual.frames * actual.samples.size()));
    return V(O{{"peak_absolute_error", V(peak)}, {"rms_error", V(rms)}, {"frames", V(actual.frames)},
               {"sample_rate", V(actual.rate)}, {"channels", V(actual.samples.size())},
               {"within_tolerance", V(peak <= *limits["peak"].number() && rms <= *limits["rms"].number())}});
}

V factoryCatalog(const Module& module, std::string_view pluginId) {
    const auto* factory = static_cast<const clap_preset_discovery_factory_t*>(
        module.entry->get_factory(CLAP_PRESET_DISCOVERY_FACTORY_ID));
    O result;
    if (!factory) return V(std::move(result));
    for (unsigned i = 0; i < factory->count(factory); ++i) {
        const auto* desc = factory->get_descriptor(factory, i);
        if (!desc || desc->id != pluginId) continue;
        bool declared{};
        const clap_preset_discovery_indexer_t indexer{CLAP_VERSION, "Cell release host", "Cell", "", "1", &declared,
            [](const auto*, const auto*) { return true; },
            [](const clap_preset_discovery_indexer_t* host, const clap_preset_discovery_location_t* location) {
                if (location && location->kind == CLAP_PRESET_DISCOVERY_LOCATION_PLUGIN)
                    *static_cast<bool*>(host->indexer_data) = true;
                return true;
            },
            [](const auto*, const auto*) { return true; }, [](const auto*, const char*) -> const void* { return nullptr; }};
        const auto* provider = factory->create(factory, &indexer, desc->id);
        require(provider, "Could not create the plug-in's preset discovery provider");
        const auto destroy = [](const clap_preset_discovery_provider_t* item) { item->destroy(item); };
        const std::unique_ptr<const clap_preset_discovery_provider_t, decltype(destroy)> owned(provider, destroy);
        if (!provider->init(provider) || !declared) continue;
        struct Collection { O entries; std::string current; bool invalid{}; } collection;
        clap_preset_discovery_metadata_receiver_t receiver{};
        receiver.receiver_data = &collection;
        receiver.begin_preset = [](const auto* sink, const char* label, const char* key) {
            auto& out = *static_cast<Collection*>(sink->receiver_data);
            if (!label || !key || !*key || out.entries.size() >= 4096) { out.invalid = true; return false; }
            try {
                out.current = key;
                return out.entries.emplace(key, V(O{{"name", V(label)}})).second;
            } catch (...) { out.invalid = true; return false; }
        };
        receiver.add_plugin_id = [](const auto*, const auto*) {};
        receiver.add_extra_info = [](const auto* sink, const char* key, const char* value) {
            auto& out = *static_cast<Collection*>(sink->receiver_data);
            if (!key || !value || std::string_view(key) != "bank" || out.current.empty()) return;
            try {
                auto record = *out.entries.at(out.current).object();
                record.emplace("bank", V(value));
                out.entries.at(out.current) = V(std::move(record));
            } catch (...) { out.invalid = true; }
        };
        require(provider->get_metadata(provider, CLAP_PRESET_DISCOVERY_LOCATION_PLUGIN, nullptr, &receiver) &&
                !collection.invalid, "Could not read the released factory preset catalog");
        result.merge(collection.entries);
    }
    return V(std::move(result));
}

void preserve(const V& previous, const V& current, std::string_view surface) {
    require(previous.object() && current.object(), "Invalid released identity map");
    for (const auto& [key, value] : *previous.object())
        if (json::write(value) != json::write(current[key]))
            throw std::runtime_error(std::string(surface) + " changed: " + key);
}

void recall(const Plugin& plugin, std::string_view key) {
    const auto* load = plugin.extension<clap_plugin_preset_load_t>(CLAP_EXT_PRESET_LOAD);
    require(load->from_location(plugin.instance, CLAP_PRESET_DISCOVERY_LOCATION_PLUGIN, nullptr,
                                std::string(key).c_str()), "Could not recall the released factory preset");
}

std::string renderFilename(std::size_t index) { return "render-" + std::to_string(index) + ".wav"; }

File caseState(const File& root, std::size_t index) {
    return root.getChildFile("state-" + std::to_string(index) + ".bin");
}

V capture(const Module& module, const File& root, const V& fixture) {
    Plugin identity(module, {});
    const auto* id = identity.instance->desc->id;
    const auto mapping = parameterMap(identity.instance, identity.extension<clap_plugin_params_t>(CLAP_EXT_PARAMS));
    const auto layout = audioLayout(identity);
    const auto catalog = factoryCatalog(module, id);
    const auto input = root.getChildFile("input.wav");
    if (!input.existsAsFile()) writeAudio(input, stimulus(layout.inputs.empty() ? 1 : layout.inputs.front()));
    const auto source = readAudio(input);
    const auto points = automation(fixture["automation"], mapping, source.frames);
    A cases;
    auto stateCase = [&](const char* kind, const File& provided, const Point& setting, bool playAutomation) {
        Plugin original(module, id);
        if (provided.existsAsFile()) original.load(provided);
        else original.set(setting.parameter, setting.value);
        const auto observed = values(original, mapping);
        const auto index = cases.size();
        original.save(caseState(root, index));
        Plugin restored(module, id);
        restored.load(caseState(root, index));
        preserve(observed, values(restored, mapping), "Saved parameter value");
        auto rendered = render(restored, layout, source, playAutomation ? points : std::vector<Point>{});
        writeAudio(root.getChildFile(renderFilename(index)), rendered.signal);
        cases.emplace_back(O{{"kind", V(kind)}, {"values", observed},
                             {"checkpoints", V(std::move(rendered.checkpoints))}});
    };
    stateCase("session", root.getChildFile("provided-session.bin"), points.back(), true);
    stateCase("saved-preset", root.getChildFile("provided-preset.bin"), points.front(), false);
    A keys;
    if (const auto* requested = fixture["factory_presets"].array()) keys = *requested;
    if (keys.empty() && !catalog.object()->empty()) keys.emplace_back(catalog.object()->begin()->first);
    require(keys.size() <= 32, "Too many reference factory presets");
    std::set<std::string> used;
    for (const auto& item : keys) {
        const std::string key(item.text());
        require(!key.empty() && !catalog[key].isNull() && used.insert(key).second,
                "Factory preset key is absent or duplicated");
        Plugin preset(module, id);
        recall(preset, key);
        const auto observed = values(preset, mapping);
        const auto rendered = render(preset, layout, source, {});
        writeAudio(root.getChildFile(renderFilename(cases.size())), rendered.signal);
        cases.emplace_back(O{{"kind", V("factory-preset")}, {"key", V(key)}, {"values", observed}});
    }

    return V(O{{"schema", V("cell.clap-release-baseline.v1")}, {"id", V(id)},
               {"version", V(identity.instance->desc->version ? identity.instance->desc->version : "")},
               {"mapping", mapping}, {"ports", layout.description}, {"factory_catalog", catalog},
               {"automation", automationJson(points)}, {"cases", V(std::move(cases))},
               {"tolerances", fixture["tolerances"]}});
}

bool verify(const Module& module, const File& root, O& report) {
    const auto baseline = readJson(root.getChildFile("session.json"));
    require(baseline["schema"].text() == "cell.clap-release-baseline.v1" && !baseline["id"].text().empty(),
            "Invalid release session baseline");
    const auto limits = baseline["tolerances"];
    const auto* peak = limits["peak"].number();
    const auto* rms = limits["rms"].number();
    require(peak && rms && *peak >= 0 && *rms >= 0, "Invalid released render tolerances");
    A checks;
    // Keep each completed check in the report even if a later state or render fails.
    const auto passed = [&](const char* name, std::size_t count) {
        checks.emplace_back(O{{"name", V(name)}, {"status", V("passed")}, {"count", V(count)}});
        report["checks"] = V(checks);
    };
    const std::string id(baseline["id"].text());
    Plugin identity(module, id);
    report["plugin_id"] = V(id);
    report["previous_version"] = baseline["version"];
    report["candidate_version"] = V(identity.instance->desc->version ? identity.instance->desc->version : "");
    report["tolerances"] = limits;
    passed("plug-in-id", 1);

    const auto mapping = parameterMap(identity.instance, identity.extension<clap_plugin_params_t>(CLAP_EXT_PARAMS));
    preserve(baseline["mapping"], mapping, "Released CLAP parameter mapping");
    passed("parameter-mappings", baseline["mapping"].object()->size());
    const auto layout = audioLayout(identity);
    require(json::write(baseline["ports"]) == json::write(layout.description), "Released audio layout changed");
    passed("audio-layout", 1);
    preserve(baseline["factory_catalog"], factoryCatalog(module, id), "Released factory preset mapping");
    passed("factory-catalog", baseline["factory_catalog"].object()->size());

    const auto* cases = baseline["cases"].array();
    require(cases && cases->size() >= 2 && cases->size() <= 34 &&
            cases->at(0)["kind"].text() == "session" && cases->at(1)["kind"].text() == "saved-preset",
            "Invalid release render cases");
    const auto source = readAudio(root.getChildFile("input.wav"));
    const auto points = automation(baseline["automation"], mapping, source.frames);
    bool success = true;
    A results;
    for (std::size_t index = 0; index < cases->size(); ++index) {
        const auto& fixture = (*cases)[index];
        Plugin plugin(module, id);
        const auto kind = fixture["kind"].text();
        if (index < 2) plugin.load(caseState(root, index));
        else {
            require(kind == "factory-preset" && !fixture["key"].text().empty(), "Invalid release factory reference");
            recall(plugin, fixture["key"].text());
        }
        preserve(fixture["values"], values(plugin, mapping), "Restored state or preset parameter");
        const auto rendered = render(plugin, layout, source, index == 0 ? points : std::vector<Point>{});
        if (index == 0) {
            require(fixture["checkpoints"].array() &&
                    json::write(fixture["checkpoints"]) == json::write(V(rendered.checkpoints)),
                    "Released automation reached different parameter values");
            passed("automation", points.size());
        }
        const auto metrics = measure(rendered.signal, readAudio(root.getChildFile(renderFilename(index))), limits);
        auto result = *metrics.object();
        result.emplace("kind", V(kind));
        if (index >= 2) result.emplace("factory_key", fixture["key"]);
        results.emplace_back(std::move(result));
        report["renders"] = V(results);
        success &= *metrics["within_tolerance"].boolean();
    }
    passed("saved-session", 1);
    passed("preset-recall", cases->size() - 1);
    return success;
}

int run(const std::vector<fs::path>& args) try {
    require(args.size() == 6 && (args[1] == "capture" || args[1] == "verify"),
            "Usage: cell-release-clap capture|verify MODULE BUNDLE BASELINE FIXTURE_OR_REPORT");
    const auto captureMode = args[1] == "capture";
    const File root(args[4]);
    if (captureMode) {
        const Module module(args[2], args[3]);
        writeJson(root.getChildFile("session.json"), capture(module, root, readJson(File(args[5]))));
        return 0;
    }

    O report;
    bool success = false;
    try {
        const Module module(args[2], args[3]);
        success = verify(module, root, report);
        if (!success) report["error"] = V("Offline render exceeds the released tolerances");
    } catch (const std::exception& error) {
        report["error"] = V(error.what());
    }
    report["status"] = V(success ? "passed" : "failed");
    writeJson(File(args[5]), V(std::move(report)));
    return success ? 0 : 1;
} catch (const std::exception& error) {
    std::cerr << error.what() << '\n';
    return 1;
}
} // namespace

#if defined(_WIN32)
int wmain(int argc, wchar_t** argv) {
#else
int main(int argc, char** argv) {
#endif
    return run(std::vector<fs::path>(argv, argv + argc));
}
