// Original Cell Framework implementation; see LICENSE.
#include <cell/files.hpp>
#include <cell/graph_project.hpp>
#include <cell/json.hpp>
#include <cell/plugin.hpp>
#include <clap/clap.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstring>
#include <filesystem>
#include <iostream>

#ifdef _WIN32
#include <windows.h>
#else
#include <dlfcn.h>
#endif

namespace {
using namespace cell;
using Value = json::Value;
using Samples = std::array<std::vector<float>, 2>;
namespace fs = std::filesystem;
constexpr unsigned rate = 48000, blockFrames = 128, frames = 4097;
constexpr double peakLimit = 1e-6, rmsLimit = 1e-7;
constexpr ProgramAddress factorySlot{4, 2};

void require(bool ok, const char* message) {
    if (!ok) throw std::runtime_error(message);
}

void writeJson(const File& file, const Value& value) {
    const auto text = json::write(value);
    files::atomicWrite(file.nativePath(), std::as_bytes(std::span(text)));
}

Value readJson(const File& file) {
    const auto bytes = files::readBounded(file.nativePath(), 8 * 1024 * 1024);
    return json::read({reinterpret_cast<const char*>(bytes.data()), bytes.size()});
}

struct Module {
    explicit Module(const fs::path& path) {
#ifdef _WIN32
        handle = LoadLibraryW(path.c_str());
        if (handle) entry = reinterpret_cast<const clap_plugin_entry_t*>(GetProcAddress(handle, "clap_entry"));
#else
        handle = dlopen(path.c_str(), RTLD_NOW | RTLD_LOCAL);
        if (handle) entry = static_cast<const clap_plugin_entry_t*>(dlsym(handle, "clap_entry"));
#endif
        const auto utf8 = path.u8string();
        if (!entry || !entry->init(reinterpret_cast<const char*>(utf8.c_str()))) {
            close();
            throw std::runtime_error("Could not load the installed CLAP plug-in");
        }
    }
    ~Module() { entry->deinit(); close(); }
    Module(const Module&) = delete;
    Module& operator=(const Module&) = delete;

    void close() {
        if (!handle) return;
#ifdef _WIN32
        FreeLibrary(handle);
#else
        dlclose(handle);
#endif
    }
#ifdef _WIN32
    HMODULE handle{};
#else
    void* handle{};
#endif
    const clap_plugin_entry_t* entry{};
};

const clap_host_t host{
    CLAP_VERSION, nullptr, "Cell compatibility host", "Cell", "", "1",
    [](const clap_host_t*, const char*) -> const void* { return nullptr; },
    [](const clap_host_t*) {}, [](const clap_host_t*) {}, [](const clap_host_t*) {}
};

clap_event_param_value_t edit(clap_id id, unsigned offset, double value) {
    return {{sizeof(clap_event_param_value_t), offset, CLAP_CORE_EVENT_SPACE_ID, CLAP_EVENT_PARAM_VALUE, 0},
            id, nullptr, -1, -1, -1, -1, value};
}

clap_input_events_t inputEvents(std::vector<clap_event_param_value_t>& events) {
    return {&events, [](const clap_input_events_t* input) {
        return static_cast<std::uint32_t>(static_cast<const std::vector<clap_event_param_value_t>*>(input->ctx)->size());
    }, [](const clap_input_events_t* input, std::uint32_t index) -> const clap_event_header_t* {
        return &static_cast<const std::vector<clap_event_param_value_t>*>(input->ctx)->at(index).header;
    }};
}

Value parameterMap(const clap_plugin_t* plugin, const clap_plugin_params_t* params) {
    Value::Object result;
    for (std::uint32_t index = 0; index < params->count(plugin); ++index) {
        clap_param_info_t info{};
        require(params->get_info(plugin, index, &info), "CLAP parameter enumeration failed");
        const auto inserted = result.emplace(std::to_string(info.id), Value(Value::Object{
            {"name", Value(info.name)}, {"module", Value(info.module)}, {"flags", Value(info.flags)},
            {"minimum", Value(info.min_value)}, {"maximum", Value(info.max_value)},
            {"default", Value(info.default_value)}
        })).second;
        require(inserted, "CLAP reported duplicate parameter IDs");
    }
    require(!result.empty(), "CLAP reported no parameters");
    return Value(std::move(result));
}

struct Plugin {
    Plugin(const Module& module, const std::string& id) {
        const auto* factory = static_cast<const clap_plugin_factory_t*>(module.entry->get_factory(CLAP_PLUGIN_FACTORY_ID));
        require(factory && factory->get_plugin_count(factory) == 1, "CLAP factory is missing the reference plug-in");
        const auto* descriptor = factory->get_plugin_descriptor(factory, 0);
        require(descriptor && descriptor->id, "CLAP descriptor has no ID");
        require(id.empty() || id == descriptor->id, "Released CLAP plug-in ID changed");
        instance = factory->create_plugin(factory, &host, descriptor->id);
        require(instance != nullptr, "Could not instantiate the saved CLAP plug-in ID");
        if (!instance->init(instance)) {
            instance->destroy(instance);
            throw std::runtime_error("Could not initialize the saved CLAP plug-in");
        }
    }
    ~Plugin() { instance->destroy(instance); }
    Plugin(const Plugin&) = delete;
    Plugin& operator=(const Plugin&) = delete;

    template<class Extension>
    const Extension* extension(const char* id) const {
        const auto* result = static_cast<const Extension*>(instance->get_extension(instance, id));
        require(result != nullptr, id);
        return result;
    }

    void set(clap_id id, double value) const {
        std::vector events{edit(id, 0, value)};
        const auto input = inputEvents(events);
        extension<clap_plugin_params_t>(CLAP_EXT_PARAMS)->flush(instance, &input, nullptr);
    }

    double value(clap_id id) const {
        double result{};
        require(extension<clap_plugin_params_t>(CLAP_EXT_PARAMS)->get_value(instance, id, &result),
                "Saved parameter ID is missing");
        return result;
    }

    void save(const File& file) const {
        std::vector<std::byte> data;
        const clap_ostream_t stream{&data, [](const clap_ostream_t* out, const void* src, std::uint64_t size) -> std::int64_t {
            const auto* bytes = static_cast<const std::byte*>(src);
            auto& buffer = *static_cast<std::vector<std::byte>*>(out->ctx);
            buffer.insert(buffer.end(), bytes, bytes + size);
            return static_cast<std::int64_t>(size);
        }};
        require(extension<clap_plugin_state_t>(CLAP_EXT_STATE)->save(instance, &stream) && !data.empty(),
                "CLAP state capture failed");
        files::atomicWrite(file.nativePath(), data);
    }

    void load(const File& file) const {
        auto data = files::readBounded(file.nativePath(), 1024 * 1024);
        std::span<const std::byte> remaining(data);
        const clap_istream_t stream{&remaining, [](const clap_istream_t* in, void* dst, std::uint64_t size) -> std::int64_t {
            auto& bytes = *static_cast<std::span<const std::byte>*>(in->ctx);
            const auto count = std::min<std::uint64_t>(size, bytes.size());
            if (count) std::memcpy(dst, bytes.data(), count);
            bytes = bytes.subspan(static_cast<std::size_t>(count));
            return static_cast<std::int64_t>(count);
        }};
        require(extension<clap_plugin_state_t>(CLAP_EXT_STATE)->load(instance, &stream) && remaining.empty(),
                "Candidate could not read the saved state or preset");
    }

    const clap_plugin_t* instance{};
};

Samples readWave(const File& path, std::int64_t expectedFrames) {
    const auto reader = audio::openAudioFile(path);
    require(reader && reader->info().channels == 2 && reader->info().sampleRate == rate &&
            reader->info().frames == expectedFrames, "Saved WAVE shape changed");
    Samples samples{std::vector<float>(expectedFrames), std::vector<float>(expectedFrames)};
    float* planes[]{samples[0].data(), samples[1].data()};
    require(reader->read(planes, 2, 0, static_cast<int>(expectedFrames)), "Could not decode the referenced WAVE");
    return samples;
}

void writeWave(const File& file, const Samples& samples) {
    audio::WaveFileOptions options;
    options.sampleRate = rate;
    options.channels = 2;
    options.bitsPerSample = 32;
    options.floatingPoint = true;
    options.timeReferenceSamples = 1024;
    auto writer = audio::createWaveFile(file, options);
    const float* planes[]{samples[0].data(), samples[1].data()};
    require(writer->write(planes, 2, static_cast<int>(samples[0].size())) && writer->finish().wasOk(),
            "Could not save a reference WAVE");
}

Samples render(const Plugin& plugin, const File& asset, const Value::Array& automation) {
    auto source = readWave(asset, frames);
    Samples output{std::vector<float>(frames), std::vector<float>(frames)};
    const auto* instance = plugin.instance;
    require(plugin.extension<clap_plugin_render_t>(CLAP_EXT_RENDER)->set(instance, CLAP_RENDER_OFFLINE),
            "The CLAP plug-in refused offline mode");
    require(instance->activate(instance, rate, 1, blockFrames), "CLAP activation failed");
    struct RenderLifetime {
        const clap_plugin_t* instance;
        bool processing{};
        ~RenderLifetime() {
            if (processing) instance->stop_processing(instance);
            instance->deactivate(instance);
        }
    } lifetime{instance, instance->start_processing(instance)};
    require(lifetime.processing, "CLAP start_processing failed");
    std::size_t next{};
    for (unsigned position = 0; position < frames; position += blockFrames) {
        const auto count = std::min(blockFrames, frames - position);
        std::vector<clap_event_param_value_t> edits;
        while (next != automation.size()) {
            const auto& point = automation[next];
            const auto* value = point["value"].number();
            // All admitted sample positions and CLAP IDs fit exactly in a double.
            const auto sampleNumber = point["sample"].number();
            const auto parameterNumber = point["parameter"].number();
            require(sampleNumber && parameterNumber && value && *sampleNumber >= position &&
                    *sampleNumber < frames && *parameterNumber >= 0 && *parameterNumber < CLAP_INVALID_ID &&
                    std::trunc(*sampleNumber) == *sampleNumber && std::trunc(*parameterNumber) == *parameterNumber,
                    "Saved automation point is invalid");
            if (*sampleNumber >= position + count) break;
            edits.push_back(edit(static_cast<clap_id>(*parameterNumber),
                                 static_cast<unsigned>(*sampleNumber) - position, *value));
            ++next;
        }
        const auto events = inputEvents(edits);
        float* inputs[]{source[0].data() + position, source[1].data() + position};
        float* outputs[]{output[0].data() + position, output[1].data() + position};
        clap_audio_buffer_t in{}, out{};
        in.channel_count = out.channel_count = 2;
        in.data32 = inputs;
        out.data32 = outputs;
        clap_process_t process{};
        process.steady_time = position;
        process.frames_count = count;
        process.audio_inputs = &in;
        process.audio_outputs = &out;
        process.audio_inputs_count = process.audio_outputs_count = 1;
        process.in_events = &events;
        require(instance->process(instance, &process) != CLAP_PROCESS_ERROR, "CLAP offline processing failed");
    }
    require(next == automation.size(), "Saved automation was not delivered");
    return output;
}

void compare(const Samples& actual, const File& reference) {
    const auto expected = readWave(reference, frames);
    double peak{}, squares{};
    for (unsigned channel = 0; channel < 2; ++channel)
        for (unsigned frame = 0; frame < frames; ++frame) {
            require(std::isfinite(actual[channel][frame]) && std::isfinite(expected[channel][frame]),
                    "Offline render contains non-finite audio");
            const auto error = std::abs(double(actual[channel][frame]) - expected[channel][frame]);
            peak = std::max(peak, error);
            squares += error * error;
        }
    const auto rms = std::sqrt(squares / (2 * frames));
    std::cout << reference.getFileName().toStdString() << " peak=" << peak << " rms=" << rms << '\n';
    require(peak <= peakLimit && rms <= rmsLimit, "CLAP offline reference is outside its tolerance");
}

Value presetMetadata(Engine& engine, const File* destination) {
    auto* memory = dynamic_cast<ProgramMemory*>(&engine.processor());
    require(memory && memory->recallProgram(engine, factorySlot), "Saved factory preset address could not be recalled");
    const auto catalog = memory->programs(engine.parameters());
    require(catalog.active == factorySlot, "Active bank or preset number changed");
    Value::Array banks;
    for (const auto& bank : catalog.banks) {
        Value::Array programs;
        for (const auto& program : bank.programs)
            programs.emplace_back(Value::Object{{"number", Value(program.number)}, {"name", Value(program.name)}});
        banks.emplace_back(Value::Object{{"number", Value(bank.number)}, {"name", Value(bank.name)},
                                        {"programs", Value(std::move(programs))}});
    }
    if (destination) files::atomicWrite(destination->nativePath(), engine.saveState());
    return Value(Value::Object{{"bank", Value(factorySlot.bank)}, {"program", Value(factorySlot.program)},
                              {"catalog", Value(std::move(banks))}});
}

constexpr std::array graphExamples{"example.json", "compensated.json", "intent.json", "modulation.json"};

void graphProjects(const File& root, const File& asset, bool capture, const File& examples) {
    for (unsigned index = 0; index < graphExamples.size(); ++index) {
        const auto stem = "graph-v" + std::to_string(index + 1);
        const auto archive = root.getChildFile(stem + ".cellgraph");
        const auto document = root.getChildFile(stem + ".json");
        const auto rendered = root.getChildFile(stem + ".wav");
        if (capture) {
            auto fixture = readJson(examples.getChildFile(graphExamples[index]));
            auto fields = *fixture.object();
            if (const auto* automation = fixture["automation"].array()) {
                auto points = *automation;
                for (unsigned i = 0; i < points.size(); ++i) {
                    auto point = *points[i].object();
                    point["sample"] = Value(19 + i * 173);
                    points[i] = Value(std::move(point));
                }
                fields["automation"] = Value(std::move(points));
            }
            const auto parsed = graph::Document::parse(json::write(Value(std::move(fields))));
            graph::saveProject(archive, parsed, asset);
            require(document.replaceWithText(Text(parsed.serialize())), "Could not save the baseline graph document");
            graph::renderProject(graph::openProject(archive), rendered);
            continue;
        }
        const auto project = graph::openProject(archive);
        const auto savedDocument = document.loadFileAsString();
        const auto parsed = graph::Document::parse(savedDocument.utf8());
        require(project.graph.serialize() == parsed.serialize() &&
                json::write(json::read(savedDocument.utf8())) == json::write(json::read(parsed.serialize())),
                "Candidate changed the saved graph document");
        const auto extracted = root.getChildFile(stem + "-extracted.wav");
        graph::extractProjectWave(archive, extracted);
        require(files::readBounded(extracted.nativePath(), 1024 * 1024) == files::readBounded(asset.nativePath(), 1024 * 1024),
                "Packed asset is not byte-for-byte identical to the relocated source");
        const auto reference = audio::openAudioFile(rendered);
        require(reference && reference->info().sampleRate == rate && reference->info().channels == 2,
                "Archived graph render format changed");
        const auto result = graph::compareProjectRender(project, *reference, peakLimit);
        std::cout << stem << " peak=" << result.maximumError << " rms=" << result.rmsError << '\n';
        require(result.matches && result.rmsError <= rmsLimit, "Graph render is outside its tolerance");
    }
}

void run(const fs::path& pluginPath, const File& root, bool capture, const File& examples) {
    const auto sessionFile = root.getChildFile("session.json");
    const auto saved = capture ? Value() : readJson(sessionFile);
    const auto id = saved["plugin"].text();
    require(capture || !id.empty(), "Saved project is missing the plug-in ID");
    const Module module(pluginPath);
    Plugin plugin(module, std::string(id));
    const auto* params = plugin.extension<clap_plugin_params_t>(CLAP_EXT_PARAMS);
    const auto mapping = parameterMap(plugin.instance, params);
    const auto state = root.getChildFile("session.state");
    const auto preset = root.getChildFile("factory.preset");
    const auto mix = root.getChildFile("host-render.wav");
    const auto presetRender = root.getChildFile("preset-render.wav");
    Engine engine(plugin::descriptor(), plugin::createProcessor());
    const auto factory = presetMetadata(engine, capture ? &preset : nullptr);

    if (capture) {
        const auto gain = stableId("gain");
        const auto* flags = mapping[std::to_string(gain)]["flags"].number();
        require(flags && (static_cast<clap_param_info_flags>(*flags) & CLAP_PARAM_IS_AUTOMATABLE),
                "Reference gain parameter is missing or cannot automate");
        const auto asset = root.getChildFile("assets/input.wav");
        require(asset.getParentDirectory().createDirectory().wasOk(), "Could not create the fixture asset directory");
        Samples signal{std::vector<float>(frames), std::vector<float>(frames)};
        for (unsigned i = 0; i < frames; ++i) {
            signal[0][i] = float(int(i % 127) - 63) / 128;
            signal[1][i] = float(int(i % 53) - 26) / 64;
        }
        writeWave(asset, signal);
        plugin.set(gain, -12);
        require(plugin.value(gain) == -12, "Reference session did not apply the saved parameter");
        plugin.save(state);
        Value::Array automation{
            Value(Value::Object{{"sample", Value(19)}, {"parameter", Value(gain)}, {"value", Value(3)}}),
            Value(Value::Object{{"sample", Value(275)}, {"parameter", Value(gain)}, {"value", Value(-5)}})
        };
        writeWave(mix, render(plugin, asset, automation));
        Plugin presetInstance(module, plugin.instance->desc->id);
        presetInstance.load(preset);
        writeWave(presetRender, render(presetInstance, asset, {}));
        writeJson(sessionFile, Value(Value::Object{
            {"plugin", Value(plugin.instance->desc->id)}, {"parameters", mapping},
            {"asset", Value("assets/input.wav")}, {"gain", Value(gain)},
            {"initial", Value(-12)}, {"presetGain", Value(presetInstance.value(gain))},
            {"factory", factory}, {"automation", Value(std::move(automation))}
        }));
        graphProjects(root, asset, true, examples);
        return;
    }

    const auto* previousParameters = saved["parameters"].object();
    require(previousParameters && !previousParameters->empty(), "Saved parameter map is missing");
    for (const auto& [id, previous] : *previousParameters)
        require(json::write(previous) == json::write(mapping[id]),
                "Released CLAP parameter IDs, range, defaults or flags changed");
    require(json::write(factory) == json::write(saved["factory"]), "Factory bank or preset mapping changed");
    const auto gain = saved["gain"].number();
    const auto initial = saved["initial"].number();
    const auto presetGain = saved["presetGain"].number();
    const auto* automation = saved["automation"].array();
    require(gain && initial && presetGain && automation && !automation->empty() &&
            *gain >= 0 && *gain < CLAP_INVALID_ID && std::trunc(*gain) == *gain,
            "Saved host project is invalid");
    require(*gain == stableId("gain"), "The stable SDK ID for the reference gain parameter changed");
    require(saved["asset"].text() == "assets/input.wav", "Saved project did not retain its relative asset reference");
    const auto asset = root.getChildFile(Text(saved["asset"].text()));
    require(asset.existsAsFile(), "Referenced asset is missing after relocation");
    plugin.load(state);
    require(plugin.value(static_cast<clap_id>(*gain)) == *initial, "Saved session state restored the wrong parameter value");
    compare(render(plugin, asset, *automation), mix);
    Plugin presetInstance(module, std::string(id));
    presetInstance.load(preset);
    require(presetInstance.value(static_cast<clap_id>(*gain)) == *presetGain, "Saved preset restored the wrong parameter value");
    compare(render(presetInstance, asset, {}), presetRender);
    graphProjects(root, asset, false, examples);
}
}

int main(int argc, char** argv) {
    try {
        require(argc == 5, "usage: cell_session_reference capture|verify CLAP PROJECT GRAPH_EXAMPLES");
        const std::string_view mode(argv[1]);
        require(mode == "capture" || mode == "verify", "Invalid compatibility mode");
        run(fs::path(argv[2]), File(fs::path(argv[3])), mode == "capture", File(fs::path(argv[4])));
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
