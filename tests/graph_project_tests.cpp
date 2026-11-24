// Original Cell Framework implementation; see LICENSE.
#include <cell/files.hpp>
#include <cell/graph_project.hpp>
#include <cell/uuid.hpp>
#include <array>
#include <iostream>

namespace {
using namespace cell;

void check(bool ok, const char* message) {
    if (!ok)
        throw std::runtime_error(message);
}

template<class Action>
void rejects(Action action, std::string_view detail) {
    try {
        action();
    } catch (const std::runtime_error& error) {
        check(std::string_view(error.what()).find(detail) != std::string_view::npos, error.what());
        return;
    }
    throw std::runtime_error("Invalid graph project was accepted");
}

void test(const File& root) {
    const auto graph = graph::Document::parse(R"({
        "schema":"cell.graph.v3","sampleRate":48000,
        "nodes":[{"id":"gain","type":"gain","gainDb":0}],
        "connections":[{"from":"input","to":"gain"},{"from":"gain","to":"output"}],
        "intents":[{"id":"level","name":"Level","bindings":[
            {"node":"gain","parameter":"gainDb","atZero":-6,"atOne":0}]}],
        "automation":[{"sample":2,"intent":"level","value":1}]
    })");
    const auto wave = root.getChildFile("source.wav");
    audio::WaveFileOptions options;
    options.channels = 1;
    options.timeReferenceSamples = 128;
    std::array<float, 17003> original{};
    original[0] = .25f;
    original.back() = -.5f;
    const float* input[]{original.data()};
    {
        auto writer = audio::createWaveFile(wave, options);
        check(writer->write(input, 1, static_cast<int>(original.size())) && writer->finish().wasOk(),
              "Could not create source audio");
    }

    const auto first = root.getChildFile("first.cellgraph");
    const auto second = root.getChildFile("second.cellgraph");
    graph::saveProject(first, graph, wave);
    graph::saveProject(second, graph, wave);
    const auto bytes = files::readBounded(first.nativePath(), 128 * 1024);
    check(bytes == files::readBounded(second.nativePath(), 128 * 1024),
          "Identical project inputs produced different containers");

    rejects([&] { graph::saveProject(first, graph, wave); }, "");
    check(bytes == files::readBounded(first.nativePath(), 128 * 1024), "Failed save overwrote the project");
    const auto cancelled = root.getChildFile("cancelled.cellgraph");
    const auto filesBefore = root.findChildFiles(File::findFiles, false).size();
    rejects([&] {
        graph::saveProject(cancelled, graph, wave, [&] {
            if (root.findChildFiles(File::findFiles, false).size() > filesBefore)
                throw std::runtime_error("cancelled");
        });
    }, "cancelled");
    check(!cancelled.exists() && root.findChildFiles(File::findFiles, false).size() == filesBefore,
          "Cancelled save leaked a project or a temporary file");

    const auto waveBytes = files::readBounded(wave.nativePath(), 128 * 1024);
    const auto extracted = root.getChildFile("extracted.wav");
    graph::extractProjectWave(first, extracted);
    check(waveBytes == files::readBounded(extracted.nativePath(), 128 * 1024),
          "Extraction changed the original WAVE bytes");
    check(wave.deleteFile(), "Could not remove source fixture");
    const auto edited = graph.withIntent("level", .5, 32);
    const auto revised = root.getChildFile("revised.cellgraph");
    graph::saveProjectWithGraph(revised, first, edited);
    check(graph::openProject(revised).graph.serialize() == edited.serialize(),
          "Project revision lost the replacement graph");
    const auto revisedWave = root.getChildFile("revised.wav");
    graph::extractProjectWave(revised, revisedWave);
    check(waveBytes == files::readBounded(revisedWave.nativePath(), 128 * 1024),
          "Project revision changed the embedded WAVE bytes");
    rejects([&] { graph::saveProjectWithGraph(first, first, edited); }, "");
    check(bytes == files::readBounded(first.nativePath(), 128 * 1024),
          "Project revision modified its input archive");

    const auto failedRevision = root.getChildFile("failed-revision.cellgraph");
    auto wrongRate = edited.serialize();
    wrongRate.replace(wrongRate.find("48000"), 5, "96000");
    const auto incompatible = graph::Document::parse(wrongRate);
    rejects([&] { graph::saveProjectWithGraph(failedRevision, first, incompatible); }, "sample rate");
    rejects([&] {
        graph::saveProjectWithGraph(failedRevision, first, edited, [] { throw std::runtime_error("cancelled"); });
    }, "cancelled");
    check(!failedRevision.exists(), "Failed revision published an archive");
    const auto reopened = graph::openProject(first);
    check(reopened.graph.serialize() == graph.serialize() && reopened.graph.intents().size() == 1,
          "Project changed the editable graph");
    check(reopened.source->info().timeReferenceSamples == 128 &&
          reopened.source->info().frames == static_cast<std::int64_t>(original.size()),
          "Embedded WAVE metadata changed");

    std::array<float, 17003> decoded{};
    float* output[]{decoded.data()};
    check(reopened.source->read(output, 1, 0, static_cast<int>(decoded.size())) && decoded == original,
          "Embedded audio differs from the original");
    std::array<float, 1> last{};
    float* tail[]{last.data()};
    check(reopened.source->read(tail, 1, static_cast<std::int64_t>(original.size() - 1), 1) &&
          last.front() == original.back(), "Embedded WAVE could not seek backwards");

    const auto referenceFile = root.getChildFile("reference.wav");
    graph::renderProject(reopened, referenceFile);
    const auto reference = audio::openAudioFile(referenceFile);
    const auto exact = graph::compareProjectRender(reopened, *reference);
    check(exact.matches && !exact.firstDifference && exact.maximumError == 0 && exact.rmsError == 0,
          "Render did not match its reference");

    const auto alteredFile = root.getChildFile("altered.wav");
    decoded[513] = .125f;
    const float* altered[]{decoded.data()};
    {
        auto writer = audio::createWaveFile(alteredFile, options);
        check(writer->write(altered, 1, static_cast<int>(decoded.size())) && writer->finish().wasOk(),
              "Could not write comparison fixture");
    }
    auto alteredReader = audio::openAudioFile(alteredFile);
    const auto mismatch = graph::compareProjectRender(reopened, *alteredReader);
    check(!mismatch.matches && mismatch.firstDifference && mismatch.firstDifference->frame == 513 &&
          mismatch.firstDifference->channel == 0 && mismatch.maximumError == .125 &&
          std::abs(mismatch.rmsError - .125 / std::sqrt(double(decoded.size()))) < 1e-12,
          "Regression report missed the changed sample or measured the wrong error");
    const auto tolerated = graph::compareProjectRender(reopened, *alteredReader, .125);
    check(tolerated.matches && tolerated.firstDifference, "Tolerance hid or rejected a permitted difference");
    check(!graph::compareProjectRender(reopened, *alteredReader, .124).matches,
          "Tolerance allowed a sample above the limit");
    rejects([&] { (void)graph::compareProjectRender(reopened, *alteredReader, -1); }, "tolerance");

    const auto shorter = root.getChildFile("shorter.wav");
    {
        auto writer = audio::createWaveFile(shorter, options);
        check(writer->write(altered, 1, 1) && writer->finish().wasOk(), "Could not write short reference");
    }
    auto wrongLength = audio::openAudioFile(shorter);
    rejects([&] { (void)graph::compareProjectRender(reopened, *wrongLength); }, "frame count");

    for (const auto blockFrames : {31u, 512u, 1024u}) {
        const auto profile = graph::profileProject(reopened, blockFrames);
        check(profile.frames == static_cast<std::int64_t>(original.size()) &&
              profile.blockFrames == blockFrames && profile.channels == 1 && profile.sampleRate == 48000 &&
              profile.latencyFrames == 0 && profile.blocks == (original.size() + blockFrames - 1) / blockFrames,
              "Incorrect profile frame or block accounting");
        check(profile.slowestBlock && profile.slowestBlock->frames > 0 &&
              profile.slowestBlock->frames <= blockFrames &&
              profile.slowestBlock->firstFrame % blockFrames == 0 &&
              profile.slowestBlock->firstFrame + profile.slowestBlock->frames <= profile.frames &&
              profile.slowestBlock->nanoseconds >= 0 &&
              profile.processingNanoseconds >= profile.slowestBlock->nanoseconds &&
              profile.overBudgetBlocks <= profile.blocks &&
              profile.peakBudgetPercent >= profile.averageBudgetPercent,
              "Invalid profile timing or deadline accounting");
    }
    rejects([&] { (void)graph::profileProject(reopened, 0); }, "Profile block size");
    rejects([&] {
        (void)graph::profileProject(reopened, 128, [] { throw std::runtime_error("cancelled"); });
    }, "cancelled");

    const auto corrupted = root.getChildFile("corrupt.cellgraph");
    auto changed = bytes;
    changed[changed.size() - 33] ^= std::byte{1};
    check(corrupted.replaceWithData(changed.data(), changed.size()), "Failed to create corrupted project");
    rejects([&] { (void)graph::openProject(corrupted); }, "SHA-256 mismatch");
    rejects([&] { graph::saveProjectWithGraph(failedRevision, corrupted, edited); }, "SHA-256 mismatch");
    check(!failedRevision.exists(), "Corrupt input published a revised project");
    const auto failedExtraction = root.getChildFile("failed.wav");
    rejects([&] { graph::extractProjectWave(corrupted, failedExtraction); }, "SHA-256 mismatch");
    check(!failedExtraction.exists(), "Corrupt project published an extracted wave");
    check(corrupted.replaceWithData(bytes.data(), bytes.size() - 1), "Failed to create truncated project");
    rejects([&] { (void)graph::openProject(corrupted); }, "payload sizes");
}

void multichannelProject(const File& root) {
    const auto graph = graph::Document::parse(R"({
        "schema":"cell.graph.v2","sampleRate":48000,
        "nodes":[{"id":"layout","type":"channelMap","sources":[1,0,2,null,4,5]}],
        "connections":[{"from":"input","to":"layout"},{"from":"layout","to":"output"}],
        "automation":[]
    })");
    const auto input = root.getChildFile("surround.wav");
    const auto project = root.getChildFile("surround.cellgraph");
    const auto output = root.getChildFile("surround-render.wav");
    audio::WaveFileOptions options;
    options.channels = 6;
    options.channelMask = 0x3f;
    options.timeReferenceSamples = 48;
    const std::array<float, 6> source{.125f, .25f, .375f, .5f, .625f, .75f};
    std::array<const float*, 6> inputs;
    for (std::size_t c = 0; c < inputs.size(); ++c)
        inputs[c] = &source[c];
    {
        auto writer = audio::createWaveFile(input, options);
        check(writer->write(inputs.data(), 6, 1) && writer->finish().wasOk(), "Could not write surround source");
    }
    graph::saveProject(project, graph, input);
    const auto restored = graph::openProject(project);
    graph::renderProject(restored, output);
    const auto render = audio::openAudioFile(output);
    check(render->info().channels == 6 && render->info().channelMask == options.channelMask &&
          render->info().timeReferenceSamples == options.timeReferenceSamples,
          "Surround render lost channel layout or time reference");
    std::array<float, 6> decoded{};
    std::array<float*, 6> channels;
    for (std::size_t c = 0; c < channels.size(); ++c)
        channels[c] = &decoded[c];
    check(render->read(channels.data(), 6, 0, 1) &&
          decoded == std::array<float, 6>{.25f, .125f, .375f, 0, .625f, .75f},
          "Surround project lane order changed");
    check(graph::compareProjectRender(restored, *render).matches, "Surround render failed its own reference check");
}

} // namespace

int main() {
    File root;
    try {
        root = File::getSpecialLocation(File::tempDirectory).getChildFile("cell-project-" + Uuid().toString());
        check(root.createDirectory().wasOk(), "Could not create private project test workspace");
        test(root);
        multichannelProject(root);
        check(root.deleteRecursively(), "Could not clean up project test workspace");
    } catch (const std::exception& error) {
        if (root.exists())
            root.deleteRecursively();
        std::cerr << error.what() << '\n';
        return 1;
    }
}
