#pragma once

#include <cell/plugin.hpp>
#include <cell/parameter_timeline.hpp>
#include <mutex>

namespace cell::wrapper {

// Format-independent host boundary. Construction, preparation and state access
// are control-thread work; start, stop and process never lock or allocate.
enum class Phase : std::uint8_t { unprepared, ready, active, processing, maintenance };
enum class Result : std::uint8_t {
    ok, wrongPhase, busy, invalidConfiguration, invalidBuffer, invalidEvent,
    eventOverflow, stateTooLarge, stateRejected, processorFailure
};

struct Limits {
    static constexpr std::uint32_t channels = 64;
    static constexpr std::uint32_t hostFrames = 1u << 20;
    static constexpr std::uint32_t renderFrames = 8192;
    static constexpr std::size_t parameters = 4096;
    // Leave space for all-sound-off/all-notes-off on all 16 channels after a
    // rejected MIDI block. Engine's prepared MIDI capacity is 2048.
    static constexpr std::size_t midi = 2016;
    static constexpr std::size_t panic = 32;
    static constexpr std::size_t stateBytes = 128u * 1024u * 1024u;
};

struct Block {
    std::uint32_t frames{};
    std::span<const float* const> inputs;
    std::span<float* const> outputs;
    std::span<const float* const> sidechain;
    std::span<const ParameterEvent> parameters;
    std::span<const MidiEvent> midi;
    Transport transport;
    ParameterTimeline* timeline{};
    std::uint64_t timelineToken{};
};

class Runtime {
public:
    Runtime(const PluginDescriptor&, std::unique_ptr<Processor>);
    Runtime(const Runtime&) = delete;
    Runtime& operator=(const Runtime&) = delete;

    // Inactive, non-audio. Larger host blocks are split into prepared chunks;
    // maxFrames controls the preferred chunk size, not a silent audio cutoff.
    Result configure(const ProcessSpec&) noexcept;
    Result activate() noexcept;
    Result deactivate() noexcept;
    Result reset() noexcept;
    Result start() noexcept;
    Result stop() noexcept;
    Result process(const Block&) noexcept;
    // An adapter that cannot admit a complete host MIDI batch must reject that
    // batch and request recovery before accepting the next block.
    void requestMidiPanic() noexcept { if (engine_.descriptor().acceptsMidi) panicPending_.store(true, std::memory_order_release); }

    Phase phase() const noexcept;
    Engine& engine() noexcept { return engine_; }
    const Engine& engine() const noexcept { return engine_; }
    // The Engine address remains stable for editor/command bindings. Callers
    // must not prepare/reset/process it independently of this runtime.
    Result saveState(std::vector<std::byte>&) noexcept;
    Result loadState(std::span<const std::byte>) noexcept;
    // Host control thread: temporarily exclude render admission for a codec
    // that needs a stopped processor. Fails immediately if a callback is in
    // flight; never waits for audio. Successful restore cancels older timed
    // parameter packets before rendering is admitted again.
    Result loadStateAtIdle(std::span<const std::byte> state, ParameterTimeline* timeline = nullptr) noexcept;

private:
    using Gate = std::uint64_t;
    static_assert(std::atomic<Gate>::is_always_lock_free);
    static constexpr Gate phaseMask = 7, busyBit = 8, generationStep = 16;
    static Phase phaseOf(Gate value) noexcept { return Phase(value & phaseMask); }
    static Gate next(Gate value, Phase phase) noexcept {
        return ((value & ~Gate(15)) + generationStep) | (value & busyBit) | Gate(phase);
    }
    Result transition(Phase from, Phase to, bool allowBusy = false) noexcept;
    Result enterMaintenance(Gate&) noexcept;
    void leaveMaintenance(Gate previous, Phase phase) noexcept;
    Result reject(const Block&, Result, bool panic) noexcept;

    Engine engine_;
    std::mutex control_; // never acquired by start/stop/deactivate/process
    std::atomic<Gate> gate_{Gate(Phase::unprepared)};
    std::atomic<bool> panicPending_{};
    ProcessSpec spec_{};
    AudioBuffer<float> audio_, side_;
    std::array<ParameterEvent, Limits::parameters> events_{}, eventScratch_{};
    std::array<MidiEvent, Limits::midi + Limits::panic> midi_{}, midiScratch_{};
};

} // namespace cell::wrapper
