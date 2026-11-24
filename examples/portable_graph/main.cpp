// Original Cell Framework implementation; see LICENSE.
#include "processor.hpp"
#include <cell/audio_file.hpp>
#include <cell/files.hpp>
#include <cell/graph_project.hpp>
#include <cell/json.hpp>
#include <charconv>
#include <cmath>
#include <cell/uuid.hpp>
#include <iostream>

namespace {

cell::graph::Document readGraph(const char* path) {
    const auto bytes = cell::files::readBounded(std::filesystem::path(path), 8u * 1024u * 1024u);
    return cell::graph::Document::parse(
        {bytes.empty() ? "" : reinterpret_cast<const char*>(bytes.data()), bytes.size()});
}


std::vector<std::optional<float>> readSpeakerLayout(const char* path) {
    const auto bytes = cell::files::readBounded(std::filesystem::path(path), 64u * 1024u);
    const auto layout = cell::json::read(
        {bytes.empty() ? "" : reinterpret_cast<const char*>(bytes.data()), bytes.size()});
    const auto* slots = layout.array();
    if (!slots || slots->size() > 64)
        throw std::invalid_argument("Expected a speaker layout array with at most 64 slots");

    std::vector<std::optional<float>> degrees;
    degrees.reserve(slots->size());
    for (const auto& slot : *slots) {
        if (slot.isNull()) {
            degrees.push_back(std::nullopt);
        } else {
            const auto* angle = slot.number();
            if (!angle || !std::isfinite(*angle) || *angle < -180 || *angle > 180)
                throw std::invalid_argument("Expected a speaker angle from -180 to 180 or a null slot");
            degrees.push_back(static_cast<float>(*angle));
        }
    }
    return degrees;
}

void describe(const cell::graph::Document& document,
              const cell::audio::AudioFileInfo* source = nullptr) {
    using cell::json::Value;
    Value::Array controls;
    for (const auto& control : document.controls()) {
        Value::Object fields{
            {"node", Value(control.node)}, {"parameter", Value(std::string(control.key))},
            {"meaning", Value(std::string(control.meaning))}, {"unit", Value(std::string(control.unit))},
            {"minimum", Value(control.minimum)}, {"maximum", Value(control.maximum)},
            {"initial", Value(control.initial)}};
        if (control.mapping) {
            const auto& map = *control.mapping;
            fields.emplace("mapping", Value(Value::Object{
                {"curve", Value("exponential")}, {"target", Value(std::string(map.target))},
                {"unit", Value(std::string(map.unit))}, {"minimum", Value(map.minimum)},
                {"maximum", Value(map.maximum)}}));
        }
        controls.emplace_back(std::move(fields));
    }
    const auto graph = cell::json::read(document.serialize());
    Value::Object result{
        {"schema", Value("cell.graph-controls.v1")}, {"graphSchema", graph["schema"]},
        {"sampleRate", Value(document.sampleRate())}, {"latencySamples", Value(document.latency())},
        {"tailSamples", Value(document.tail())},
        {"requiredChannels", document.requiredChannels() ? Value(document.requiredChannels()) : Value{}},
        {"controls", Value(std::move(controls))}, {"automation", graph["automation"]},
        {"intents", graph["intents"].array() ? graph["intents"] : Value(Value::Array{})}};

    if (graph["tempoMap"].array())
        result.emplace("tempoMap", graph["tempoMap"]);
    if (graph["lfos"].array())
        result.emplace("lfos", graph["lfos"]);
    if (graph["modulation"].array())
        result.emplace("modulation", graph["modulation"]);

    Value::Array layouts;
    for (const auto& layout : document.azimuthLayouts()) {
        Value::Array speakers;
        for (const auto& degrees : layout.speakerDegrees)
            speakers.push_back(degrees ? Value(static_cast<double>(*degrees)) : Value{});
        Value::Object fields{
            {"node", Value(layout.node)}, {"sourceChannel", Value(layout.sourceChannel)},
            {"speakerDegrees", Value(std::move(speakers))},
            {"distanceMeters", Value(static_cast<double>(layout.distanceMeters))}};
        if (layout.interpolatesAzimuth)
            fields.emplace("azimuthCurve", Value("shortestArc"));
        if (layout.interpolatesDistance)
            fields.emplace("distanceCurve", Value("linear"));
        layouts.emplace_back(std::move(fields));
    }
    if (!layouts.empty())
        result.emplace("azimuthLayouts", Value(std::move(layouts)));
    if (graph["speakerLayout"].array())
        result.emplace("speakerLayout", graph["speakerLayout"]);
    if (!graph["listenerYawCurve"].isNull())
        result.emplace("listenerYawCurve", graph["listenerYawCurve"]);
    if (const auto yaw = document.listenerYaw())
        result.emplace("listenerYaw", Value(static_cast<double>(*yaw)));
    if (source) {
        result.emplace("source", Value(Value::Object{
            {"format", Value(source->format)}, {"frames", Value(source->frames)},
            {"sampleRate", Value(source->sampleRate)}, {"channels", Value(source->channels)},
            {"bitsPerSample", Value(source->bitsPerSample)},
            {"validBitsPerSample", Value(source->validBitsPerSample)},
            {"floatingPoint", Value(source->floatingPoint)}, {"channelMask", Value(source->channelMask)},
            {"timeReferenceSamples", source->timeReferenceSamples ? Value(*source->timeReferenceSamples) : Value{}}}));
    }
    std::cout << cell::json::write(Value(std::move(result))) << '\n';
}

std::uint64_t sampleArgument(std::string_view text) {
    std::uint64_t sample{};
    const auto parsed = std::from_chars(text.data(), text.data() + text.size(), sample);
    if (parsed.ec != std::errc{} || parsed.ptr != text.data() + text.size())
        throw std::invalid_argument("Automation sample must be a nonnegative 64-bit integer");
    return sample;
}

double valueArgument(std::string_view text) {
    const auto value = cell::json::read(text);
    if (!value.number())
        throw std::invalid_argument("Expected a JSON number");
    return *value.number();
}

void saveGraph(const cell::graph::Document& document, const char* path) {
    const auto& saved = document.serialize();
    cell::files::atomicWrite(std::filesystem::path(path), std::as_bytes(std::span(saved.data(), saved.size())));
}

void automate(int argc, char** argv) {
    std::vector<cell::graph::AutomationPoint> points;
    for (int i = 4; i < argc; i += 4)
        points.push_back({argv[i], argv[i + 1], sampleArgument(argv[i + 2]), valueArgument(argv[i + 3])});
    saveGraph(readGraph(argv[2]).withAutomation(points), argv[3]);
}


void render(const cell::graph::Project& project, const char* destination) {
    cell::graph::renderProject(project, cell::File(destination));
    const auto& info = project.source->info();
    std::cout << "Rendered " << info.frames + project.graph.renderFlushFrames() << " frames at "
              << info.sampleRate << " Hz\n";
}

int checkRender(const cell::graph::Project& project, const char* reference, double tolerance) {
    using cell::json::Value;
    const auto wave = cell::audio::openWaveStream(cell::File(reference).createInputStream());
    const auto result = cell::graph::compareProjectRender(project, *wave, tolerance);
    Value first;
    if (result.firstDifference)
        first = Value(Value::Object{
            {"frame", Value(result.firstDifference->frame)},
            {"channel", Value(result.firstDifference->channel)}});
    const Value summary(Value::Object{
        {"schema", Value("cell.graph-render-comparison.v1")},
        {"matches", Value(result.matches)}, {"bitExact", Value(!result.firstDifference)},
        {"frames", Value(result.frames)}, {"channels", Value(result.channels)},
        {"tolerance", Value(tolerance)}, {"maximumError", Value(result.maximumError)},
        {"rmsError", Value(result.rmsError)}, {"firstDifference", std::move(first)}});
    std::cout << cell::json::write(summary) << '\n';
    return result.matches ? 0 : 1;
}


void profile(const cell::graph::Project& project, int argc, char** argv) {
    using cell::json::Value;
    std::vector<std::uint32_t> sizes;
    for (int i = 3; i < argc; ++i) {
        const auto frames = sampleArgument(argv[i]);
        if (frames == 0 || frames > static_cast<unsigned>(std::numeric_limits<int>::max()))
            throw std::invalid_argument("Profile block size must be a positive frame count");
        sizes.push_back(static_cast<std::uint32_t>(frames));
    }
    if (sizes.empty())
        sizes.push_back(512);

    Value::Array runs;
    for (const auto frames : sizes) {
        const auto result = cell::graph::profileProject(project, frames);
        Value slowest;
        if (result.slowestBlock) {
            const auto& block = *result.slowestBlock;
            slowest = Value(Value::Object{{"firstFrame", Value(block.firstFrame)},
                {"frames", Value(block.frames)}, {"nanoseconds", Value(block.nanoseconds)}});
        }
        runs.emplace_back(Value::Object{
            {"frames", Value(result.frames)}, {"channels", Value(result.channels)},
            {"sampleRate", Value(result.sampleRate)}, {"latencyFrames", Value(result.latencyFrames)},
            {"tailFrames", Value(result.tailFrames)},
            {"blockFrames", Value(result.blockFrames)}, {"blocks", Value(result.blocks)},
            {"overBudgetBlocks", Value(result.overBudgetBlocks)},
            {"processingNanoseconds", Value(result.processingNanoseconds)},
            {"averageBudgetPercent", Value(result.averageBudgetPercent)},
            {"peakBudgetPercent", Value(result.peakBudgetPercent)}, {"slowestBlock", std::move(slowest)}});
    }
    std::cout << cell::json::write(Value(Value::Object{
        {"schema", Value("cell.graph-render-profile.v1")}, {"runs", Value(std::move(runs))}})) << '\n';
}

} // namespace

int main(int argc, char** argv) {
    using namespace cell;
    try {
        const std::string_view mode = argc > 1 ? argv[1] : "";
        if (mode == "describe" && argc == 3) {
            describe(readGraph(argv[2]));
            return 0;
        }
        if (mode == "automate" && argc >= 8 && (argc - 4) % 4 == 0) {
            automate(argc, argv);
            return 0;
        }
        if (mode == "modulation-depth" && argc >= 8 && (argc - 4) % 4 == 0) {
            std::vector<graph::ModulationDepthEdit> edits;
            edits.reserve((argc - 4) / 4);
            for (int i = 4; i < argc; i += 4)
                edits.push_back({argv[i], argv[i + 1], argv[i + 2], valueArgument(argv[i + 3])});
            saveGraph(readGraph(argv[2]).withModulationDepths(edits), argv[3]);
            return 0;
        }
        if (mode == "listener-yaw" && argc >= 6 && (argc - 4) % 2 == 0) {
            std::vector<graph::ListenerYawPoint> points;
            points.reserve((argc - 4) / 2);
            for (int index = 4; index < argc; index += 2)
                points.push_back({sampleArgument(argv[index]), valueArgument(argv[index + 1])});
            saveGraph(readGraph(argv[2]).withListenerYaw(points), argv[3]);
            return 0;
        }
        if (mode == "intent" && argc == 7) {
            saveGraph(readGraph(argv[2]).withIntent(argv[4], valueArgument(argv[6]),
                sampleArgument(argv[5])), argv[3]);
            return 0;
        }
        if (mode == "pack" && argc == 5) {
            graph::saveProject(File(argv[4]), readGraph(argv[2]), File(argv[3]));
            return 0;
        }
        if (mode == "retarget" && argc == 5) {
            const auto layout = readSpeakerLayout(argv[3]);
            saveGraph(readGraph(argv[2]).withSpeakerLayout(layout), argv[4]);
            return 0;
        }
        if (mode == "replace-graph" && argc == 5) {
            graph::saveProjectWithGraph(File(argv[4]), File(argv[2]), readGraph(argv[3]));
            return 0;
        }
        if (mode == "describe-project" && argc == 3) {
            const auto project = graph::openProject(File(argv[2]));
            describe(project.graph, &project.source->info());
            return 0;
        }
        if (mode == "verify-project" && argc == 3) {
            const auto project = graph::openProject(File(argv[2]));
            const auto& info = project.source->info();
            std::cout << "Verified graph project: " << info.frames << " frames at "
                      << info.sampleRate << " Hz, " << info.channels << " channel(s)\n";
            return 0;
        }
        if (mode == "extract-graph" && argc == 4) {
            saveGraph(graph::openProject(File(argv[2])).graph, argv[3]);
            return 0;
        }
        if (mode == "extract-wave" && argc == 4) {
            graph::extractProjectWave(File(argv[2]), File(argv[3]));
            return 0;
        }
        if (mode == "render-project" && argc == 4) {
            render(graph::openProject(File(argv[2])), argv[3]);
            return 0;
        }
        if (mode == "check-render" && (argc == 4 || argc == 5))
            return checkRender(graph::openProject(File(argv[2])), argv[3], argc == 5 ? valueArgument(argv[4]) : 0);

        if (mode == "profile-project" && argc >= 3) {
            profile(graph::openProject(File(argv[2])), argc, argv);
            return 0;
        }
        const bool live = mode == "live" && argc == 8;
        if (!live && (argc != 5 || (mode != "headless" && mode != "engine")))
            throw std::invalid_argument(
                "Usage: cell_graph_demo describe graph.json\n"
                "       cell_graph_demo pack graph.json input.wav project.cellgraph\n"
                "       cell_graph_demo describe-project project.cellgraph\n"
                "       cell_graph_demo verify-project project.cellgraph\n"
                "       cell_graph_demo retarget scene.json speakers.json output.json\n"
                "       cell_graph_demo replace-graph project.cellgraph edited.json revised.cellgraph\n"
                "       cell_graph_demo extract-graph project.cellgraph graph.json\n"
                "       cell_graph_demo extract-wave project.cellgraph source.wav\n"
                "       cell_graph_demo render-project project.cellgraph output.wav\n"
                "       cell_graph_demo profile-project project.cellgraph [block-frames ...]\n"
                "       cell_graph_demo check-render project.cellgraph reference.wav [tolerance]\n"
                "       cell_graph_demo listener-yaw scene.json output.json sample degrees [sample degrees ...]\n"
                "       cell_graph_demo intent graph.json output.json id sample value\n"
                "       cell_graph_demo automate graph.json output.json node parameter sample value [...]\n"
                "       cell_graph_demo modulation-depth graph.json output.json source node parameter depth [...]\n"
                "       cell_graph_demo headless|engine graph.json input.wav output.wav\n"
                "       cell_graph_demo live graph.json input.wav output.wav replacement.json brightness-node saved.json");

        auto document = readGraph(argv[2]);
        const auto input = audio::openAudioFile(File(argv[3]));
        if (mode == "headless") {
            render({std::move(document), input}, argv[4]);
            return 0;
        }
        const auto& info = input->info();
        if (info.sampleRate != document.sampleRate() || info.channels < 1 || info.channels > 64)
            throw std::invalid_argument("Input must have 1–64 channels at the graph sample rate");
        if (document.requiredChannels() && document.requiredChannels() != info.channels)
            throw std::invalid_argument("Input channel count does not match the graph width");

        const ProcessSpec spec{info.sampleRate, 512, info.channels};
        std::optional<Engine> engine;
        std::optional<graph::LiveSession> session;
        std::optional<graph::Document> replacement;
        if (live) {
            if (info.frames <= 3 * spec.maxFrames)
                throw std::invalid_argument("Live demo needs more than 1536 input frames");
            replacement.emplace(readGraph(argv[5]));
            if (replacement->sampleRate() != spec.sampleRate)
                throw std::invalid_argument("Replacement must use the graph sample rate");
            if (replacement->requiredChannels() && replacement->requiredChannels() != info.channels)
                throw std::invalid_argument("Replacement graph width does not match the input");
            if (replacement->latency() != document.latency())
                throw std::invalid_argument("Live replacement must keep the graph latency");
            session.emplace(document, spec);
        } else {
            auto descriptor = example::graphDescriptor;
            descriptor.channels = info.channels;
            engine.emplace(descriptor, std::make_unique<example::GraphProcessor>(document));
            engine->prepare(spec.sampleRate, spec.maxFrames, spec.channels);
        }

        if (info.frames > std::numeric_limits<std::int64_t>::max() - document.renderFlushFrames())
            throw std::length_error("Graph latency would exceed the render length limit");
        const auto outputFrames = info.frames + document.renderFlushFrames();
        const File destination(argv[4]);
        const File saved = live ? File(argv[7]) : File{};
        if (live && (saved == destination || saved.exists()))
            throw std::invalid_argument("Choose a distinct new path for the saved graph");
        if (destination.exists())
            throw std::invalid_argument("Choose a new output path");
        const auto temporary = destination.getSiblingFile(".cell-render-" + Uuid().toString());
        struct Cleanup {
            const File& file;
            bool active{};
            ~Cleanup() { if (active) file.deleteFile(); }
        } cleanup{temporary};
        auto writer = audio::createWaveFile(temporary,
            {info.sampleRate, info.channels, 32, true, info.channelMask, info.timeReferenceSamples});
        cleanup.active = true;
        AudioBuffer<float> buffer;
        buffer.resize(spec.channels, spec.maxFrames);
        for (std::int64_t offset = 0; offset < outputFrames;) {
            const auto remaining = (offset < info.frames ? info.frames : outputFrames) - offset;
            const auto count = static_cast<int>(std::min<std::int64_t>(spec.maxFrames, remaining));
            const auto block = buffer.block().slice(0, static_cast<unsigned>(count));
            if (offset >= info.frames) {
                block.clear();
            } else if (!input->read(buffer.getArrayOfWritePointers(), spec.channels, offset, count)) {
                throw std::runtime_error(input->status().getErrorMessage().toStdString());
            }
            if (session) {
                if (offset == spec.maxFrames && !session->setBrightness(argv[6], .85, offset + 37))
                    throw std::runtime_error("Could not queue brightness edit");
                if (offset == 2 * spec.maxFrames && !session->replace(std::move(*replacement)))
                    throw std::runtime_error("Could not queue replacement");
                if (offset == 3 * spec.maxFrames && !session->undo())
                    throw std::runtime_error("Could not undo replacement");
                if (!session->process(block))
                    throw std::runtime_error("Live graph rejected the audio block");
                session->reclaim();
            } else {
                engine->process(block);
            }
            if (!writer->write(buffer.getArrayOfReadPointers(), spec.channels, count))
                throw std::runtime_error(writer->status().getErrorMessage().toStdString());
            offset += count;
        }
        const auto result = writer->finish();
        if (!result)
            throw std::runtime_error(result.getErrorMessage().toStdString());
        writer.reset();
        // Publish exclusively, including when another process claimed the name during render.
        const auto published = files::publish(temporary.nativePath(), destination.nativePath(), false);
        if (!published)
            throw std::runtime_error(published.getErrorMessage().toStdString());
        if (session) {
            const auto& json = session->document().serialize();
            files::atomicWrite(saved.nativePath(), std::as_bytes(std::span(json.data(), json.size())));
        }
        std::cout << "Rendered " << outputFrames << " frames at " << info.sampleRate << " Hz\n";
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
