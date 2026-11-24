// Original Cell Framework implementation; see LICENSE.
#include <cell/graph_project.hpp>
#include <cell/files.hpp>
#include <cell/hash.hpp>
#include <cell/uuid.hpp>
#include <bit>
#include <chrono>
#include <cstring>

namespace cell::graph {
namespace {

constexpr std::string_view magic = "CELLGP01";
constexpr std::size_t headerSize = 24, digestSize = 32, graphLimit = 8u * 1024 * 1024;
using Header = std::array<std::byte, headerSize>;

void require(bool condition, const char* message) {
    if (!condition)
        throw std::runtime_error(message);
}

void putSize(std::span<std::byte, 8> field, std::uint64_t value) {
    for (auto& b : field) {
        b = std::byte(value & 0xff);
        value >>= 8;
    }
}

std::uint64_t getSize(std::span<const std::byte, 8> field) {
    std::uint64_t value{};
    for (std::size_t i = field.size(); i-- > 0;)
        value = (value << 8) | std::to_integer<unsigned>(field[i]);
    return value;
}

// A WAVE decoder sees just its byte range and can seek within it on the
// same open file handle used to verify the archive.
class WaveSlice final : public InputStream {
public:
    WaveSlice(std::shared_ptr<FileInputStream> stream, std::int64_t offset, std::int64_t length)
        : stream_(std::move(stream)), offset_(offset), length_(length) {}
    int read(void* data, int count) override {
        if (count <= 0 || position_ == length_)
            return 0;
        const auto size = static_cast<int>(std::min<std::int64_t>(count, length_ - position_));
        if (!stream_->setPosition(offset_ + position_))
            return 0;
        const auto actual = stream_->read(data, size);
        if (actual > 0)
            position_ += actual;
        return actual;
    }
    std::int64_t getPosition() const override { return position_; }
    std::int64_t getTotalLength() const override { return length_; }
    bool setPosition(std::int64_t position) override {
        if (position < 0 || position > length_)
            return false;
        position_ = position;
        return true;
    }
    bool isExhausted() const override { return position_ == length_; }
private:
    std::shared_ptr<FileInputStream> stream_;
    std::int64_t offset_, length_, position_{};
};

void checkSource(const Document& graph, const audio::AudioFileInfo& info) {
    require(info.sampleRate == graph.sampleRate() && info.channels >= 1 && info.channels <= 64,
            "Graph project needs 1–64 WAVE channels at the graph sample rate");
    require(graph.requiredChannels() == 0 || info.channels == graph.requiredChannels(),
            "WAVE channel count does not match the graph width");
}

void transfer(FileInputStream& stream, std::uint64_t remaining, Sha256& hash,
              files::AtomicWriter* destination, const audio::FileWorkCheck& check, Sha256* outputHash = nullptr) {
    std::array<std::byte, 65536> buffer{};
    while (remaining) {
        if (check)
            check();
        const auto bytes = std::span(buffer).first(
            static_cast<std::size_t>(std::min<std::uint64_t>(remaining, buffer.size())));
        require(stream.readFully(bytes), "Graph project audio ended early");
        hash.update(bytes);
        if (outputHash)
            outputHash->update(bytes);
        if (destination)
            destination->write(bytes);
        remaining -= bytes.size();
    }
}

void writeProjectHeader(files::AtomicWriter& output, Sha256& hash,
                        std::string_view text, std::uint64_t waveBytes) {
    require(!text.empty() && text.size() <= graphLimit, "Invalid graph project document size");
    require(waveBytes <= static_cast<std::uint64_t>(INT64_MAX) - headerSize - digestSize - text.size(),
            "Graph project exceeds signed file addressing");
    Header header{};
    std::memcpy(header.data(), magic.data(), magic.size());
    putSize(std::span(header).subspan<8, 8>(), text.size());
    putSize(std::span(header).subspan<16, 8>(), waveBytes);
    const auto graphBytes = std::as_bytes(std::span(text.data(), text.size()));
    hash.update(header);
    hash.update(graphBytes);
    output.write(header);
    output.write(graphBytes);
}

} // namespace

void saveProject(const File& destination, const Document& graph, const File& wave,
                 const audio::FileWorkCheck& check) {
    const auto& text = graph.serialize();
    std::shared_ptr<FileInputStream> stream = wave.createInputStream();
    require(stream && stream->getTotalLength() >= 12, "Cannot open graph project source WAVE");
    const auto waveBytes = static_cast<std::uint64_t>(stream->getTotalLength());
    const auto waveReader = audio::openWaveStream(
        std::make_unique<WaveSlice>(stream, 0, stream->getTotalLength()), check);
    checkSource(graph, waveReader->info());
    require(stream->setPosition(0), "Cannot rewind graph project source WAVE");

    Sha256 hash;
    files::AtomicWriter output(destination.nativePath());
    writeProjectHeader(output, hash, text, waveBytes);
    transfer(*stream, waveBytes, hash, &output, check);
    output.write(hash.finish());
    if (check)
        check();
    output.commit();
}

namespace {

Project readProject(const File& file, const audio::FileWorkCheck& check,
                    files::AtomicWriter* output, const Document* replacement = nullptr) {
    std::shared_ptr<FileInputStream> stream = file.createInputStream();
    require(stream && stream->getTotalLength() >= static_cast<std::int64_t>(headerSize + digestSize),
            "Cannot read graph project header");
    Header header{};
    require(stream->readFully(header) && std::memcmp(header.data(), magic.data(), magic.size()) == 0,
            "Unsupported graph project header");
    const auto graphBytes = getSize(std::span(header).subspan<8, 8>());
    const auto waveBytes = getSize(std::span(header).subspan<16, 8>());
    const auto payload = static_cast<std::uint64_t>(stream->getTotalLength()) - headerSize - digestSize;
    require(graphBytes > 0 && graphBytes <= graphLimit && graphBytes <= payload &&
            waveBytes >= 12 && waveBytes == payload - graphBytes,
            "Invalid graph project payload sizes");

    std::string text(static_cast<std::size_t>(graphBytes), '\0');
    const auto bytes = std::as_writable_bytes(std::span(text.data(), text.size()));
    require(stream->readFully(bytes), "Graph project document ended early");
    Sha256 hash;
    hash.update(header);
    hash.update(bytes);
    Sha256 replacementHash;
    if (replacement)
        writeProjectHeader(*output, replacementHash, replacement->serialize(), waveBytes);
    transfer(*stream, waveBytes, hash, output, check, replacement ? &replacementHash : nullptr);
    std::array<std::byte, digestSize> savedDigest{};
    require(stream->readFully(savedDigest) && savedDigest == hash.finish(), "Graph project SHA-256 mismatch");

    auto graph = Document::parse(text);
    auto source = audio::openWaveStream(std::make_unique<WaveSlice>(stream,
        static_cast<std::int64_t>(headerSize + graphBytes), static_cast<std::int64_t>(waveBytes)), check);
    checkSource(graph, source->info());
    if (replacement) {
        checkSource(*replacement, source->info());
        output->write(replacementHash.finish());
    }
    return {std::move(graph), std::move(source)};
}

} // namespace

Project openProject(const File& file, const audio::FileWorkCheck& check) {
    return readProject(file, check, nullptr);
}

void extractProjectWave(const File& project, const File& destination,
                        const audio::FileWorkCheck& check) {
    files::AtomicWriter output(destination.nativePath());
    (void)readProject(project, check, &output);
    if (check)
        check();
    output.commit();
}

void saveProjectWithGraph(const File& destination, const File& project, const Document& replacement,
                          const audio::FileWorkCheck& check) {
    files::AtomicWriter output(destination.nativePath());
    (void)readProject(project, check, &output, &replacement);
    if (check)
        check();
    output.commit();
}

namespace {

constexpr std::uint32_t renderBlockFrames = 512;

const audio::AudioFileInfo& sourceInfo(const Project& project) {
    require(bool(project.source), "Graph project source is missing");
    const auto& info = project.source->info();
    checkSource(project.graph, info);
    return info;
}

std::int64_t renderLength(const Project& project, const audio::AudioFileInfo& info) {
    require(info.frames >= 0 && info.frames <= INT64_MAX - project.graph.renderFlushFrames(),
            "Graph project render length exceeds signed file addressing");
    return info.frames + project.graph.renderFlushFrames();
}

template<bool Measure = false, class Sink>
void renderBlocks(const Project& project, const audio::AudioFileInfo& info, std::int64_t frames,
                  const audio::FileWorkCheck& check, Sink sink,
                  std::uint32_t blockFrames = renderBlockFrames) {
    const ProcessSpec spec{info.sampleRate, blockFrames, info.channels};
    Player player(project.graph, spec);
    AudioBuffer<float> buffer(spec.channels, spec.maxFrames);
    for (std::int64_t offset = 0; offset < frames;) {
        if (check)
            check();
        const auto remaining = (offset < info.frames ? info.frames : frames) - offset;
        const auto count = static_cast<int>(std::min<std::int64_t>(spec.maxFrames, remaining));
        const auto block = buffer.block().slice(0, static_cast<unsigned>(count));
        if (offset < info.frames) {
            if (!project.source->read(buffer.getArrayOfWritePointers(), spec.channels, offset, count, check))
                throw std::runtime_error(project.source->status().getErrorMessage().toStdString());
        } else {
            block.clear();
        }
        if constexpr (Measure) {
            using Clock = std::chrono::steady_clock;
            const auto start = Clock::now();
            const bool processed = player.process(block);
            const auto end = Clock::now();
            require(processed, "Graph player rejected its render block");
            sink(offset, block.frames, std::chrono::duration<double, std::nano>(end - start).count());
        } else {
            require(player.process(block), "Graph player rejected its render block");
            sink(buffer, block, offset, count);
        }
        offset += count;
    }
}

} // namespace

void renderProject(const Project& project, const File& destination, const audio::FileWorkCheck& check) {
    const auto& info = sourceInfo(project);
    const auto frames = renderLength(project, info);
    require(!destination.exists(), "Choose a new output path");
    const auto temporary = destination.getSiblingFile(".cell-render-" + Uuid().toString());
    struct Cleanup {
        const File& file;
        bool active{};
        ~Cleanup() { if (active) file.deleteFile(); }
    } cleanup{temporary};
    auto writer = audio::createWaveFile(temporary,
        {info.sampleRate, info.channels, 32, true, info.channelMask, info.timeReferenceSamples});
    cleanup.active = true;
    renderBlocks(project, info, frames, check, [&](const auto& buffer, AudioBlock<float>, std::int64_t, int count) {
        if (!writer->write(buffer.getArrayOfReadPointers(), info.channels, count, check))
            throw std::runtime_error(writer->status().getErrorMessage().toStdString());
    });
    const auto finished = writer->finish();
    if (!finished)
        throw std::runtime_error(finished.getErrorMessage().toStdString());
    writer.reset();
    if (check)
        check();
    const auto published = files::publish(temporary.nativePath(), destination.nativePath(), false);
    if (!published)
        throw std::runtime_error(published.getErrorMessage().toStdString());
}

RenderComparison compareProjectRender(const Project& project, audio::AudioFileReader& reference,
                                      double tolerance, const audio::FileWorkCheck& check) {
    require(std::isfinite(tolerance) && tolerance >= 0, "Render tolerance must be finite and nonnegative");
    const auto& info = sourceInfo(project);
    const auto frames = renderLength(project, info);
    const auto& expected = reference.info();
    require(expected.sampleRate == info.sampleRate && expected.channels == info.channels &&
            expected.frames == frames, "Reference sample rate, channels or frame count differs from the project render");

    RenderComparison result;
    result.frames = frames;
    result.channels = info.channels;
    AudioBuffer<float> referenceBuffer(info.channels, renderBlockFrames);
    double squaredError{};
    renderBlocks(project, info, frames, check, [&](const auto&, AudioBlock<float> rendered,
                                                   std::int64_t offset, int count) {
        if (!reference.read(referenceBuffer.getArrayOfWritePointers(), info.channels, offset, count, check))
            throw std::runtime_error(reference.status().getErrorMessage().toStdString());
        const auto baseline = referenceBuffer.block();
        for (unsigned i = 0; i < rendered.frames; ++i) {
            for (unsigned c = 0; c < rendered.channelCount; ++c) {
                const float actual = rendered.channel(c)[i];
                const float target = baseline.channel(c)[i];
                require(std::isfinite(actual) && std::isfinite(target), "Non-finite sample in render comparison");
                if (!result.firstDifference &&
                    std::bit_cast<std::uint32_t>(actual) != std::bit_cast<std::uint32_t>(target))
                    result.firstDifference = RenderDifference{offset + i, c};
                const double error = std::abs(double(actual) - double(target));
                result.maximumError = std::max(result.maximumError, error);
                squaredError += error * error;
            }
        }
    });
    if (frames)
        result.rmsError = std::sqrt(squaredError / (double(frames) * info.channels));
    result.matches = tolerance == 0 ? !result.firstDifference : result.maximumError <= tolerance;
    return result;
}

RenderProfile profileProject(const Project& project, std::uint32_t blockFrames,
                             const audio::FileWorkCheck& check) {
    require(blockFrames > 0 && blockFrames <= (1u << 20),
            "Profile block size must be in [1, 1048576]");
    const auto& info = sourceInfo(project);
    RenderProfile result;
    result.frames = renderLength(project, info);
    result.channels = info.channels;
    result.sampleRate = static_cast<unsigned>(info.sampleRate);
    result.blockFrames = blockFrames;
    result.latencyFrames = project.graph.latency();
    result.tailFrames = project.graph.tail();
    const auto nanosecondsPerFrame = 1e9 / info.sampleRate;

    renderBlocks<true>(project, info, result.frames, check,
        [&](std::int64_t firstFrame, std::uint32_t frames, double nanoseconds) {
            const auto budget = frames * nanosecondsPerFrame;
            ++result.blocks;
            if (nanoseconds > budget)
                ++result.overBudgetBlocks;
            result.processingNanoseconds += nanoseconds;
            result.peakBudgetPercent = std::max(result.peakBudgetPercent, 100 * nanoseconds / budget);
            if (!result.slowestBlock || nanoseconds > result.slowestBlock->nanoseconds)
                result.slowestBlock = RenderBlockTiming{firstFrame, frames, nanoseconds};
        }, blockFrames);

    if (result.frames)
        result.averageBudgetPercent = 100 * result.processingNanoseconds /
                                      (double(result.frames) * nanosecondsPerFrame);
    return result;
}

} // namespace cell::graph
