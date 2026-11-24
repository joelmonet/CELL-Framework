// Original Cell Framework implementation; see LICENSE.
#pragma once

#include <cell/audio_file.hpp>
#include <cell/portable_graph.hpp>

namespace cell::graph {

struct Project {
    Document graph;
    std::shared_ptr<audio::AudioFileReader> source;
};

// Worker-thread operations. A project owns one graph and the original bytes
// of one compatible native WAVE source. Creation publishes exclusively.
// Keep source files stable during packing and project files stable while open.
void saveProject(const File& destination, const Document&, const File& wave,
                 const audio::FileWorkCheck& = {});
// Verify the original archive and publish a new project with the replacement
// graph and a byte-for-byte copy of the embedded WAVE.
void saveProjectWithGraph(const File& destination, const File& project, const Document& replacement,
                          const audio::FileWorkCheck& = {});
Project openProject(const File&, const audio::FileWorkCheck& = {});
// Verify the complete archive and publish the untouched WAVE bytes exclusively.
void extractProjectWave(const File& project, const File& destination,
                        const audio::FileWorkCheck& = {});

struct RenderDifference {
    std::int64_t frame;
    unsigned channel;
};

struct RenderComparison {
    std::int64_t frames{};
    unsigned channels{};
    double maximumError{}, rmsError{};
    std::optional<RenderDifference> firstDifference;
    bool matches{};
};

// Render to a new float32 WAVE file. Both operations run on a worker/control
// thread and flush the graph's declared latency and finite tail.
void renderProject(const Project&, const File& destination, const audio::FileWorkCheck& = {});
// Compare decoded float32 samples. Zero tolerance uses bitwise comparison.
// A positive tolerance is an absolute per-sample threshold.
RenderComparison compareProjectRender(const Project&, audio::AudioFileReader& reference,
                                      double tolerance = 0, const audio::FileWorkCheck& = {});

struct RenderBlockTiming {
    std::int64_t firstFrame;
    std::uint32_t frames;
    double nanoseconds;
};

struct RenderProfile {
    std::int64_t frames{};
    unsigned channels{}, sampleRate{};
    std::uint32_t blockFrames{}, latencyFrames{}, tailFrames{};
    std::uint64_t blocks{}, overBudgetBlocks{};
    double processingNanoseconds{}, averageBudgetPercent{}, peakBudgetPercent{};
    std::optional<RenderBlockTiming> slowestBlock;
};

// Time only Player::process on a worker thread. The cost is elapsed time,
// including scheduler preemption, and is independent of file I/O timings.
RenderProfile profileProject(const Project&, std::uint32_t blockFrames = 512,
                             const audio::FileWorkCheck& = {});

} // namespace cell::graph
