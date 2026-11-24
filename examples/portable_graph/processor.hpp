// Original Cell Framework implementation; see LICENSE.
#pragma once

#include <cell/plugin.hpp>
#include <cell/portable_graph.hpp>

namespace cell::example {

inline const PluginDescriptor graphDescriptor{
    "com.example.cell.portable-graph", "Cell Portable Graph", "Cell SDK Example",
    "0.1.0", 2, {}, true};

class GraphProcessor final : public Processor {
public:
    explicit GraphProcessor(graph::Document document) : document_(std::move(document)) {}
    std::uint32_t latency() const noexcept override { return document_.latency(); }
    std::uint32_t tail() const noexcept override { return document_.tail(); }
    void prepare(const ProcessSpec& spec) override { player_.emplace(document_, spec); }
    void reset() noexcept override {
        if (player_)
            player_->reset();
    }
    void process(AudioBlock<float> block, const ParameterBank&, const Transport& transport,
                 std::span<const MidiEvent>) noexcept override {
        if (player_)
            player_->process(block, transport);
    }
private:
    graph::Document document_;
    std::optional<graph::Player> player_;
};

} // namespace cell::example
