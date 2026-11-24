// Original Cell Framework implementation; see LICENSE.
#include "gain.hpp"
#include "allocation_watch.hpp"
#include <cell/realtime.hpp>
#include <bit>
#include <cmath>
#include <iostream>

namespace {
using namespace cell;

void check(bool condition, const char* message) {
    if (!condition)
        throw std::runtime_error(message);
}

// Global replacements are linked only into this test executable. The scope
// marks the processing thread and restores its previous observation state.
class AudioObservation {
public:
    AudioObservation() noexcept : previous_(cell_test::audio) { cell_test::audio = true; }
    ~AudioObservation() { cell_test::audio = previous_; }
    AudioObservation(const AudioObservation&) = delete;
    AudioObservation& operator=(const AudioObservation&) = delete;
private:
    bool previous_;
};

void verifyAllocationHooks() {
    void* ordinary{};
    void* aligned{};
    const auto allocations = cell_test::allocations;
    const auto deletions = cell_test::deletions;
    {
        AudioObservation audio;
        ordinary = ::operator new(8);
        aligned = ::operator new(64, std::align_val_t{64});
        ::operator delete(ordinary);
        ::operator delete(aligned, std::align_val_t{64});
    }
    check(cell_test::allocations - allocations == 2 && cell_test::deletions - deletions == 2,
          "Real-time allocation instrumentation did not observe its own test calls");
}

void processChecked(Engine& engine, AudioBlock<float> block, unsigned rate,
                    const char* phase, std::span<const ParameterEvent> events = {}) {
    const auto allocations = cell_test::allocations;
    const auto deletions = cell_test::deletions;
    {
        AudioObservation audio;
        engine.process(block, events);
    }
    const auto allocated = cell_test::allocations - allocations;
    const auto deleted = cell_test::deletions - deletions;
    if (allocated || deleted) {
        std::cerr << "Real-time allocation check failed during " << phase << " at "
                  << rate << " Hz, " << block.frames << " frames, "
                  << block.channelCount << " channels: " << allocated
                  << " allocation(s), " << deleted << " free(s)\n";
        throw std::runtime_error("Heap activity in Engine::process");
    }
}

void exercise(unsigned rate, unsigned frames, unsigned channels) {
    Engine engine(plugin::descriptor(), plugin::createProcessor());
    engine.prepare(rate, frames, channels);
    AudioBuffer<float> audio(channels, frames);
    const auto block = audio.block();
    const auto fill = [&] {
        for (unsigned c = 0; c < channels; ++c)
            std::fill_n(block.channel(c), frames, .1f * (c + 1));
    };
    const auto eventFrame = frames / 2;
    const ParameterEvent event{eventFrame, example::gainId, -6};
    fill();
    processChecked(engine, block, rate, "automation", {&event, 1});
    for (unsigned c = 0; c < channels; ++c) {
        const float input = .1f * (c + 1);
        for (unsigned n = 0; n < eventFrame; ++n)
            check(block.channel(c)[n] == input, "Automation changed audio before its sample offset");
        check(block.channel(c)[eventFrame] < input &&
              block.channel(c)[eventFrame] > input * dbToGain(-6), "Gain ramp missed its sample offset");
    }
    const auto settleBlocks = (rate / 100 + frames - 1) / frames + 1;
    for (unsigned n = 0; n < settleBlocks; ++n) {
        fill();
        processChecked(engine, block, rate, "smoothing");
    }
    const auto verifyGain = [&] {
        for (unsigned c = 0; c < channels; ++c) {
            const float expected = (.1f * (c + 1)) * dbToGain(-6);
            for (unsigned n = 0; n < frames; ++n)
                check(std::abs(block.channel(c)[n] - expected) < 1e-6f, "Incorrect settled gain");
        }
    };
    verifyGain();

    Engine recalled(plugin::descriptor(), plugin::createProcessor());
    check(recalled.loadState(engine.saveState()), "Gain state was rejected");
    recalled.prepare(rate, frames, channels);
    fill();
    processChecked(recalled, block, rate, "recalled state");
    verifyGain();
}


#ifdef CELL_TEST_REFERENCE_GAIN_STATE
void verifyReferenceGainState() {
    // Captured from the r37 CI-template build of Cell Gain Example at -6 dB.
    // Keep the saved bytes fixed so future readers are tested against that build.
    constexpr std::array<unsigned char, 28> saved{
        0x43, 0x45, 0x4c, 0x4c, 0x01, 0x00, 0x00, 0x00,
        0x99, 0x2f, 0xe1, 0x7b, 0x01, 0x00, 0x00, 0x00,
        0xfe, 0x26, 0x54, 0x1b, 0x00, 0x00, 0x00, 0x00,
        0x00, 0x00, 0x18, 0xc0
    };
    Engine engine(plugin::descriptor(), plugin::createProcessor());
    check(engine.loadState(std::as_bytes(std::span(saved))), "Released gain state was rejected");
    check(engine.parameters().get(example::gainId) == -6, "Released state restored the wrong gain");
    engine.prepare(48000, 64, 2);
    AudioBuffer<float> audio(2, 64);
    const auto block = audio.block();
    for (unsigned channel = 0; channel < block.channelCount; ++channel)
        std::fill_n(block.channel(channel), block.frames, .25f);
    processChecked(engine, block, 48000, "released gain state");
    for (unsigned channel = 0; channel < block.channelCount; ++channel)
        for (unsigned frame = 0; frame < block.frames; ++frame)
            check(std::abs(block.channel(channel)[frame] - .25f * dbToGain(-6)) < 1e-6f,
                  "Released state rendered the wrong gain");
}
#endif

void verifySubnormalProcessing() {
#if CELL_HAS_SSE_CONTROL || ((defined(__aarch64__) || defined(__arm64__)) && (defined(__GNUC__) || defined(__clang__)))
    Engine engine(plugin::descriptor(), plugin::createProcessor());
    engine.prepare(48000, 64, 2);
    AudioBuffer<float> audio(2, 64);
    const auto block = audio.block();
    constexpr auto subnormal = std::bit_cast<float>(std::uint32_t{0x00400000});
    for (unsigned c = 0; c < 2; ++c)
        std::fill_n(block.channel(c), block.frames, subnormal);
#if CELL_HAS_SSE_CONTROL
    constexpr unsigned denormalFlags = 0x8040u;
    struct RestoreHostFlags {
        unsigned saved = _mm_getcsr();
        ~RestoreHostFlags() { _mm_setcsr(saved); }
    } hostFlags;
    _mm_setcsr(hostFlags.saved & ~denormalFlags);
#endif
    processChecked(engine, block, 48000, "subnormal input");
#if CELL_HAS_SSE_CONTROL
    check((_mm_getcsr() & denormalFlags) == 0, "Processing changed host denormal flags");
#endif
    for (unsigned c = 0; c < 2; ++c)
        for (unsigned frame = 0; frame < block.frames; ++frame)
            check(std::bit_cast<std::uint32_t>(block.channel(c)[frame]) == 0,
                  "Processing preserved a subnormal sample");
#endif
}

} // namespace

int main() {
    try {
#ifdef CELL_EXPECTED_PLUGIN_ID
        const auto* descriptorId = plugin::descriptor().id;
        check(descriptorId && std::string_view(descriptorId) == CELL_EXPECTED_PLUGIN_ID,
              "Processor plug-in ID differs from plugin-id.txt");
#endif
        verifyAllocationHooks();
#ifdef CELL_TEST_REFERENCE_GAIN_STATE
        verifyReferenceGainState();
#endif
        verifySubnormalProcessing();
        for (const unsigned rate : {44100u, 48000u, 96000u})
            for (const unsigned frames : {1u, 31u, 64u, 512u, 1024u})
                for (const unsigned channels : {1u, 2u})
                    exercise(rate, frames, channels);
        std::cout << "PASS gain automation, smoothing, and state recall, and zero observed audio-thread C++ heap activity across 30 processing configurations\n";
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
