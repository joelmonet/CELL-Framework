#include <cell/wrapper.hpp>
#include <cstring>
#include <limits>

namespace cell::wrapper {

Runtime::Runtime(const PluginDescriptor& descriptor, std::unique_ptr<Processor> processor)
    : engine_(descriptor, std::move(processor)) {}

Phase Runtime::phase() const noexcept { return phaseOf(gate_.load(std::memory_order_acquire)); }

Result Runtime::transition(Phase from, Phase to, bool allowBusy) noexcept {
    auto gate = gate_.load(std::memory_order_acquire);
    if (phaseOf(gate) != from) return Result::wrongPhase;
    if (!allowBusy && (gate & busyBit)) return Result::busy;
    return gate_.compare_exchange_strong(gate, next(gate, to), std::memory_order_acq_rel)
        ? Result::ok : Result::busy;
}

Result Runtime::enterMaintenance(Gate& previous) noexcept {
    previous = gate_.load(std::memory_order_acquire);
    if (phaseOf(previous) == Phase::processing || phaseOf(previous) == Phase::maintenance)
        return Result::wrongPhase;
    if (previous & busyBit) return Result::busy;
    auto expected = previous;
    return gate_.compare_exchange_strong(expected, next(previous, Phase::maintenance), std::memory_order_acq_rel)
        ? Result::ok : Result::busy;
}

void Runtime::leaveMaintenance(Gate previous, Phase phase) noexcept {
    // No audio reader can enter maintenance. Control operations serialize on
    // control_; start/stop/deactivate reject the maintenance phase.
    gate_.store(next(next(previous, Phase::maintenance), phase), std::memory_order_release);
}

Result Runtime::configure(const ProcessSpec& spec) noexcept {
    if (!std::isfinite(spec.sampleRate) || spec.sampleRate < 1000 || spec.sampleRate > 768000 ||
        !spec.maxFrames || spec.maxFrames > Limits::hostFrames ||
        (spec.channels != engine_.descriptor().channels &&
            !(engine_.descriptor().supportsMono && spec.channels == 1)))
        return Result::invalidConfiguration;
    std::lock_guard lock(control_);
    const auto initial = phase();
    if (initial != Phase::unprepared && initial != Phase::ready) return Result::wrongPhase;
    Gate previous{};
    if (const auto result = enterMaintenance(previous); result != Result::ok) return result;
    try {
        const ProcessSpec candidate{spec.sampleRate, std::min(spec.maxFrames, Limits::renderFrames), spec.channels};
        AudioBuffer<float> audio, side;
        audio.resize(candidate.channels, candidate.maxFrames);
        if (engine_.descriptor().supportsSidechain) side.resize(2, candidate.maxFrames);
        engine_.prepare(candidate.sampleRate, candidate.maxFrames, candidate.channels);
        audio_ = std::move(audio); side_ = std::move(side); spec_ = candidate;
        panicPending_.store(false, std::memory_order_release);
        leaveMaintenance(previous, Phase::ready);
        return Result::ok;
    } catch (...) {
        // Processor preparation can have changed its internals before throwing.
        // Require a successful preparation before any further render.
        leaveMaintenance(previous, Phase::unprepared);
        return Result::processorFailure;
    }
}

Result Runtime::activate() noexcept {
    std::lock_guard lock(control_);
    if (phase() != Phase::ready) return Result::wrongPhase;
    Gate previous{};
    if (const auto result = enterMaintenance(previous); result != Result::ok) return result;
    engine_.reset();
    panicPending_.store(false, std::memory_order_release);
    leaveMaintenance(previous, Phase::active);
    return Result::ok;
}

Result Runtime::deactivate() noexcept {
    const auto current = phase();
    if (current != Phase::active && current != Phase::processing) return Result::wrongPhase;
    // A host must join its current render before destroying the instance. This
    // closes admission immediately; the busy bit prevents reprepare meanwhile.
    return transition(current, Phase::ready, true);
}

Result Runtime::reset() noexcept {
    std::lock_guard lock(control_);
    if (phase() != Phase::ready && phase() != Phase::active) return Result::wrongPhase;
    Gate previous{};
    if (const auto result = enterMaintenance(previous); result != Result::ok) return result;
    engine_.reset();
    panicPending_.store(false, std::memory_order_release);
    leaveMaintenance(previous, phaseOf(previous));
    return Result::ok;
}

Result Runtime::start() noexcept { return transition(Phase::active, Phase::processing); }
Result Runtime::stop() noexcept { return transition(Phase::processing, Phase::active, true); }

Result Runtime::reject(const Block& block, Result result, bool panic) noexcept {
    if (panic && engine_.descriptor().acceptsMidi) panicPending_.store(true, std::memory_order_release);
    if (block.frames <= Limits::hostFrames && block.outputs.size() <= Limits::channels)
        for (auto* output : block.outputs) if (output) std::fill_n(output, block.frames, 0.f);
    return result;
}

Result Runtime::process(const Block& block) noexcept {
    auto gate = gate_.load(std::memory_order_acquire);
    if (phaseOf(gate) != Phase::processing) return reject(block, Result::wrongPhase, false);
    if ((gate & busyBit) || !gate_.compare_exchange_strong(gate, gate | busyBit, std::memory_order_acq_rel))
        return reject(block, Result::busy, false);
    struct Release {
        std::atomic<Gate>& gate;
        ~Release() { gate.fetch_and(~busyBit, std::memory_order_release); }
    } release{gate_};

    if (block.frames > Limits::hostFrames || block.outputs.size() != spec_.channels ||
        (!block.inputs.empty() && block.inputs.size() != spec_.channels) ||
        block.sidechain.size() > 2 || (!block.sidechain.empty() && !engine_.descriptor().supportsSidechain))
        return reject(block, Result::invalidBuffer, true);
    for (std::size_t channel = 0; channel < block.outputs.size(); ++channel) {
        if (!block.outputs[channel]) return reject(block, Result::invalidBuffer, true);
        for (std::size_t earlier = 0; earlier < channel; ++earlier)
            if (const auto a = reinterpret_cast<std::uintptr_t>(block.outputs[channel]), b = reinterpret_cast<std::uintptr_t>(block.outputs[earlier]);
                (a > b ? a - b : b - a) < std::size_t(block.frames) * sizeof(float))
                return reject(block, Result::invalidBuffer, true);
    }
    bool needsScratch = false;
    const auto validateAliases = [&](std::span<const float* const> inputs, bool main) noexcept {
        for (std::size_t input = 0; input < inputs.size(); ++input) if (inputs[input])
            for (std::size_t output = 0; output < block.outputs.size(); ++output) {
                const auto a = reinterpret_cast<std::uintptr_t>(inputs[input]);
                const auto b = reinterpret_cast<std::uintptr_t>(block.outputs[output]);
                // Shifted overlaps could overwrite a later chunk's input.
                // Exact same-buffer channel permutations use prepared scratch.
                if (a != b && (a > b ? a - b : b - a) < std::size_t(block.frames) * sizeof(float)) return false;
                if (main && a == b && input != output) needsScratch = true;
            }
        return true;
    };
    if (!validateAliases(block.inputs, true) || !validateAliases(block.sidechain, false))
        return reject(block, Result::invalidBuffer, true);
    if (block.parameters.size() > Limits::parameters || block.midi.size() > Limits::midi)
        return reject(block, Result::eventOverflow, true);
    if (!std::isfinite(block.transport.tempo) || block.transport.tempo <= 0 ||
        !std::isfinite(block.transport.beatPosition) || !block.transport.numerator || !block.transport.denominator ||
        block.transport.samplePosition > std::numeric_limits<std::int64_t>::max() - block.frames ||
        !std::isfinite(block.transport.beatPosition + double(block.frames) / spec_.sampleRate * block.transport.tempo / 60.))
        return reject(block, Result::invalidEvent, true);
    for (const auto& event : block.parameters)
        if (event.frame > block.frames || !std::isfinite(event.value) || !engine_.parameters().find(event.id))
            return reject(block, Result::invalidEvent, true);
    if (!block.midi.empty() && !engine_.descriptor().acceptsMidi) return reject(block, Result::invalidEvent, false);
    for (const auto& event : block.midi) {
        const auto status = event.data[0];
        const auto size = (status & 0xf0) == 0xc0 || (status & 0xf0) == 0xd0 ? 2u : 3u;
        if (event.frame >= block.frames || status < 0x80 || status >= 0xf0 || event.size != size ||
            event.data[1] > 127 || (size == 3 && event.data[2] > 127))
            return reject(block, Result::invalidEvent, true);
    }

    if (block.timeline && !block.timeline->applyThrough(block.timelineToken, engine_.parameters()))
        return reject(block, Result::invalidEvent, true);
    std::copy(block.parameters.begin(), block.parameters.end(), events_.begin());
    stableEventSort({events_.data(), block.parameters.size()}, eventScratch_);
    if (!block.frames) {
        for (std::size_t n = 0; n < block.parameters.size(); ++n)
            engine_.parameters().set(events_[n].id, events_[n].value);
        return Result::ok;
    }
    std::size_t midiCount{};
    if (panicPending_.exchange(false, std::memory_order_acq_rel))
        for (unsigned channel = 0; channel < 16; ++channel) {
            midi_[midiCount++] = {0, {std::uint8_t(0xb0 | channel), 120, 0}, 3};
            midi_[midiCount++] = {0, {std::uint8_t(0xb0 | channel), 123, 0}, 3};
        }
    std::copy(block.midi.begin(), block.midi.end(), midi_.begin() + midiCount);
    midiCount += block.midi.size();
    stableFrameSort<MidiEvent>({midi_.data(), midiCount}, midiScratch_);
    std::size_t parameterCursor{}, midiCursor{};
    for (std::uint32_t offset = 0; offset < block.frames;) {
        const auto frames = std::min(spec_.maxFrames, block.frames - offset);
        const auto end = offset + frames;
        auto audio = needsScratch ? audio_.block().slice(0, frames)
            : AudioBlock<float>{block.outputs.data(), spec_.channels, frames, offset};
        AudioBlock<float> side;
        if (!block.sidechain.empty()) {
            side = side_.block().slice(0, frames);
            side.channelCount = std::uint32_t(block.sidechain.size());
            for (std::size_t channel = 0; channel < block.sidechain.size(); ++channel) {
                if (const auto* input = block.sidechain[channel])
                    std::memcpy(side.channel(std::uint32_t(channel)), input + offset, std::size_t(frames) * sizeof(float));
                else std::fill_n(side.channel(std::uint32_t(channel)), frames, 0.f);
            }
        }
        // Preserve the sidechain before touching main outputs, since a host may
        // supply an output buffer as the key input. Ordinary in-place main
        // processing takes no extra audio copy through our scratch buffer.
        for (std::size_t channel = 0; channel < block.outputs.size(); ++channel) {
            const auto* input = block.inputs.empty() ? nullptr : block.inputs[channel];
            if (!input) std::fill_n(audio.channel(std::uint32_t(channel)), frames, 0.f);
            else if (input + offset != audio.channel(std::uint32_t(channel)))
                std::memcpy(audio.channel(std::uint32_t(channel)), input + offset, std::size_t(frames) * sizeof(float));
        }
        const auto parameterBegin = parameterCursor, midiBegin = midiCursor;
        while (parameterCursor < block.parameters.size() &&
            (events_[parameterCursor].frame < end || (end == block.frames && events_[parameterCursor].frame == end)))
            events_[parameterCursor++].frame -= offset;
        while (midiCursor < midiCount && midi_[midiCursor].frame < end) midi_[midiCursor++].frame -= offset;
        auto transport = block.transport;
        transport.samplePosition += offset;
        transport.beatPosition += double(offset) / spec_.sampleRate * transport.tempo / 60.;
        engine_.process(audio, {events_.data() + parameterBegin, parameterCursor - parameterBegin}, transport,
            {midi_.data() + midiBegin, midiCursor - midiBegin}, side);
        if (needsScratch) for (std::size_t channel = 0; channel < block.outputs.size(); ++channel)
            std::memcpy(block.outputs[channel] + offset, audio.channel(std::uint32_t(channel)), std::size_t(frames) * sizeof(float));
        offset = end;
    }
    return Result::ok;
}

Result Runtime::saveState(std::vector<std::byte>& output) noexcept {
    std::lock_guard lock(control_);
    Gate previous{};
    const bool stopped = engine_.stateSaveRequiresStoppedProcessing();
    if (stopped) if (const auto result = enterMaintenance(previous); result != Result::ok) return result;
    Result result = Result::ok;
    try {
        auto bytes = engine_.saveState();
        if (bytes.size() > Limits::stateBytes) result = Result::stateTooLarge;
        else output = std::move(bytes);
    } catch (...) { result = Result::processorFailure; }
    if (stopped) leaveMaintenance(previous, phaseOf(previous));
    return result;
}

Result Runtime::loadState(std::span<const std::byte> bytes) noexcept {
    if (bytes.size() > Limits::stateBytes) return Result::stateTooLarge;
    std::lock_guard lock(control_);
    Gate previous{};
    const bool stopped = engine_.stateRequiresStoppedProcessing();
    if (stopped) if (const auto result = enterMaintenance(previous); result != Result::ok) return result;
    Result result;
    try { result = engine_.loadState(bytes) ? Result::ok : Result::stateRejected; }
    catch (...) { result = Result::processorFailure; }
    if (stopped) leaveMaintenance(previous, phaseOf(previous));
    return result;
}

Result Runtime::loadStateAtIdle(std::span<const std::byte> bytes, ParameterTimeline* timeline) noexcept {
    if (bytes.size() > Limits::stateBytes) return Result::stateTooLarge;
    std::lock_guard lock(control_);
    auto previous = gate_.load(std::memory_order_acquire);
    if ((previous & busyBit) || phaseOf(previous) == Phase::maintenance) return Result::busy;
    if (!gate_.compare_exchange_strong(previous, next(previous, Phase::maintenance), std::memory_order_acq_rel)) return Result::busy;
    Result result;
    try {
        result = engine_.loadState(bytes) ? Result::ok : Result::stateRejected;
        if (result == Result::ok && timeline) timeline->discardPublished();
    } catch (...) { result = Result::processorFailure; }
    leaveMaintenance(previous, phaseOf(previous));
    return result;
}

} // namespace cell::wrapper
