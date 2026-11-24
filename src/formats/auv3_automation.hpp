// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <cell/plugin.hpp>

namespace cell::auv3 {

inline bool canRamp(const Parameter& parameter) noexcept {
    return parameter.automatable && !parameter.bypass && parameter.step < 1;
}

struct AutomationEvent {
    std::uint32_t frame;
    std::size_t parameter;
    double value;
    std::uint32_t duration;
};

class Automation {
public:
    Automation(std::size_t parameters, std::size_t maxEvents, std::size_t maxMidiEvents = 0) : ramps_(parameters) {
        active_.reserve(parameters);
        points_.reserve(maxEvents + parameters);
        midiScratch_.reserve(maxMidiEvents);
    }

    void reset() noexcept {
        for (const auto index : active_) ramps_[index].slot = inactive;
        active_.clear();
    }

    // Input events are already validated and ordered. The render loop owns the
    // ramp state and keeps it across blocks until it receives an edit or reset.
    void process(Engine& engine, AudioBlock<float> audio, std::span<const AutomationEvent> events,
                 Transport transport, AudioBlock<float> sidechain = {}, std::span<const MidiEvent> midiInput = {},
                 midi::Output* midiOutput = nullptr) noexcept {
        const auto& bank = engine.parameters();
        const auto fields = bank.descriptors();
        const auto sampleRate = engine.spec().sampleRate;
        for (std::uint32_t frame = 0; frame < audio.frames;) {
            points_.clear();
            const auto frameEvents = events;
            while (!events.empty() && events.front().frame == frame) {
                const auto& event = events.front();
                auto& ramp = ramps_[event.parameter];
                const auto& field = fields[event.parameter];
                const auto target = field.clamp(event.value);
                if (event.duration != 0) {
                    const auto start = ramp.slot == inactive
                        ? valueAtFrame(bank, field, ramp, frame)
                        : field.clamp(ramp.value());
                    if (start != target) startRamp(event.parameter, start, target, event.duration);
                    else setPoint(event.parameter, field.id, frame, target);
                } else {
                    setPoint(event.parameter, field.id, frame, target);
                }
                events = events.subspan(1);
            }

            for (std::size_t i = 0; i < active_.size();) {
                const auto index = active_[i];
                auto& ramp = ramps_[index];
                const auto& field = fields[index];
                const auto value = field.clamp(ramp.value());
                if (value != valueAtFrame(bank, field, ramp, frame))
                    points_.push_back({0, field.id, value});
                if (ramp.elapsed == ramp.duration) stopRamp(index);
                else { ++ramp.elapsed; ++i; }
            }

            auto next = events.empty() ? audio.frames : events.front().frame;
            if (!active_.empty()) next = frame + 1;
            auto segment = transport;
            segment.samplePosition += frame;
            if (segment.playing) segment.beatPosition += double(frame) / sampleRate * segment.tempo / 60.0;
            const auto side = sidechain.channelCount ? sidechain.slice(frame, next - frame) : AudioBlock<float>{};
            SegmentOutput output{midiOutput, frame, next - frame};
            engine.process(audio.slice(frame, next - frame), points_, segment, takeMidi(midiInput, frame, next), side,
                           midiOutput ? &output : nullptr);
            for (const auto& event : frameEvents.first(frameEvents.size() - events.size()))
                ramps_[event.parameter].pointFrame = UINT32_MAX;
            frame = next;
        }
    }

private:
    class SegmentOutput final : public midi::Output {
    public:
        SegmentOutput(midi::Output* destination, std::uint32_t offset, std::uint32_t frames) noexcept
            : destination_(destination), offset_(offset), frames_(frames) {}

        bool push(MidiEvent event) noexcept override {
            if (event.frame >= frames_) {
                reject();
                return false;
            }
            event.frame += offset_;
            return destination_->push(event);
        }

        void reject() noexcept override { destination_->reject(); }

    private:
        midi::Output* destination_;
        std::uint32_t offset_, frames_;
    };

    // Hosts supply ordered events. Consume each once and rebase only when the
    // engine is about to render a segment with a nonzero starting offset.
    std::span<const MidiEvent> takeMidi(std::span<const MidiEvent>& pending, std::uint32_t begin,
                                       std::uint32_t end) noexcept {
        std::size_t count{};
        while (count < pending.size() && pending[count].frame < end) ++count;
        const auto events = pending.first(count);
        pending = pending.subspan(count);
        if (begin == 0 || events.empty()) return events;
        midiScratch_.clear();
        for (auto event : events) {
            event.frame -= begin;
            midiScratch_.push_back(event);
        }
        return midiScratch_;
    }

    static constexpr auto inactive = std::numeric_limits<std::size_t>::max();
    struct Ramp {
        double start{}, target{};
        std::uint32_t duration{}, elapsed{};
        std::size_t slot = inactive;
        // Used when several commands for this parameter share one sample.
        std::uint32_t pointFrame = UINT32_MAX;
        double pointValue{};

        double value() const noexcept { return std::lerp(start, target, double(elapsed) / duration); }
    };

    static double valueAtFrame(const ParameterBank& bank, const Parameter& field,
                               const Ramp& ramp, std::uint32_t frame) noexcept {
        return ramp.pointFrame == frame ? ramp.pointValue : bank.get(field.id);
    }

    void setPoint(std::size_t index, ParamId id, std::uint32_t frame, double value) noexcept {
        stopRamp(index);
        auto& ramp = ramps_[index];
        ramp.pointFrame = frame;
        ramp.pointValue = value;
        points_.push_back({0, id, value});
    }

    void startRamp(std::size_t index, double start, double target, std::uint32_t duration) noexcept {
        auto& ramp = ramps_[index];
        if (ramp.slot == inactive) {
            ramp.slot = active_.size();
            active_.push_back(index);
        }
        ramp.start = start;
        ramp.target = target;
        ramp.duration = duration;
        ramp.elapsed = 0;
    }

    void stopRamp(std::size_t index) noexcept {
        auto& ramp = ramps_[index];
        if (ramp.slot == inactive) return;
        const auto last = active_.back();
        active_[ramp.slot] = last;
        ramps_[last].slot = ramp.slot;
        active_.pop_back();
        ramp.slot = inactive;
    }

    std::vector<Ramp> ramps_;
    std::vector<std::size_t> active_;
    std::vector<ParameterEvent> points_;
    std::vector<MidiEvent> midiScratch_;
};

} // namespace cell::auv3
