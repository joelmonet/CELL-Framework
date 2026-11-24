#pragma once
#include <cell/analysis.hpp>
#include <cell/dsp.hpp>
#include <cell/oversampling.hpp>
#include <cell/realtime.hpp>
#include <limits>

namespace cell::dsp {
struct FerroControls {
    float driveDb = 6, shape{}, mix = 1, outputDb = -6;
    bool bypass{}, delta{};
};

// Two odd-symmetric bounded curves. Shape 0 is tanh; shape 1 is a C1 cubic
// limiter. Drive >= 1; both return unity at a full-scale DC input.
inline double ferroCurve(double input, double drive, double shape, double inverseTanh) noexcept {
    const auto x = input * drive;
    const auto limited = std::clamp(x, -1.0, 1.0);
    const auto edge = limited * (1.5 - .5 * limited * limited);
    if (shape >= 1) return edge;
    const auto round = std::tanh(x) * inverseTanh;
    return shape <= 0 ? round : round + (edge - round) * shape;
}

class FerroKernel {
public:
    using Oversampling = GuardedOversampler<5>;
    static constexpr unsigned factor = Oversampling::factor;
    static constexpr unsigned latency = Oversampling::latency;
    static constexpr unsigned tail = Oversampling::tail;
    void prepare(const ProcessSpec& spec) {
        if (spec.channels != 2 || !std::isfinite(spec.sampleRate) || spec.sampleRate < 1000 || spec.sampleRate > 768000 || !spec.maxFrames)
            throw std::invalid_argument("invalid Ferro process specification");
        sampleRate_ = spec.sampleRate; maxFrames_ = spec.maxFrames;
        for (auto* smoother : {&drive_, &shape_, &mix_, &output_, &bypass_, &delta_}) smoother->prepare(spec.sampleRate, .01);
        reset(); prepared_ = true;
    }
    void reset() noexcept {
        for (auto& channel : channels_) { channel.oversampler.reset(); channel.dry.fill(0); channel.position = 0; }
        initialized_ = false; packetOffset_ = 0; packet_ = {}; sequence_ = 0;
    }
    void process(AudioBlock<float> block, FerroControls controls, AnalysisMailbox* mailbox = nullptr) noexcept {
        ScopedNoDenormals denormals;
        if (!prepared_ || block.channelCount != 2 || block.frames > maxFrames_) { block.clear(); return; }
        const auto finite = [](float value, float fallback, float low, float high) { return std::isfinite(value) ? std::clamp(value, low, high) : fallback; };
        const auto drive = dbToGain(finite(controls.driveDb, 6, 0, 30));
        const auto shape = finite(controls.shape, 0, 0, 1), mix = finite(controls.mix, 1, 0, 1);
        const auto output = dbToGain(finite(controls.outputDb, -6, -24, 12));
        if (!initialized_) {
            drive_.reset(drive); shape_.reset(shape); mix_.reset(mix); output_.reset(output);
            bypass_.reset(controls.bypass ? 1.f : 0.f); delta_.reset(controls.delta ? 1.f : 0.f);
            previousDrive_ = drive; previousShape_ = shape; previousNorm_ = 1 / std::tanh(double(drive)); initialized_ = true;
        }
        drive_.target(drive); shape_.target(shape); mix_.target(mix); output_.target(output);
        bypass_.target(controls.bypass ? 1.f : 0.f); delta_.target(controls.delta ? 1.f : 0.f);
        const bool analyze = mailbox && mailbox->analysisEnabled();
        if (!analyze) packetOffset_ = 0;
        for (std::uint32_t n = 0; n < block.frames; ++n) {
            const double d = drive_.next(), s = shape_.next(), m = mix_.next(), g = output_.next(), bypass = bypass_.next(), delta = delta_.next();
            const double norm = d == previousDrive_ ? previousNorm_ : 1 / std::tanh(d);
            float dryLeft{}, processedLeft{}, inputPeak{}, outputPeak{};
            for (unsigned c = 0; c < 2; ++c) {
                const auto sample = block.channel(c)[n];
                const double input = std::isfinite(sample) ? double(sample) : 0;
                auto& state = channels_[c];
                const auto dry = state.dry[state.position];
                state.dry[state.position] = input; state.position = (state.position + 1) % latency;
                unsigned phase{};
                const auto wet = state.oversampler.process(input, [&](double x) noexcept {
                    const double t = double(++phase) / factor;
                    return ferroCurve(x, previousDrive_ + (d - previousDrive_) * t,
                        previousShape_ + (s - previousShape_) * t, previousNorm_ + (norm - previousNorm_) * t);
                });
                const auto processed = (dry * (1 - m) + wet * m) * g;
                const auto audition = processed - dry * delta;
                const auto result = bypass >= 1 ? dry : bypass <= 0 ? audition : audition * (1 - bypass) + dry * bypass;
                constexpr auto maximum = double(std::numeric_limits<float>::max());
                block.channel(c)[n] = float(std::clamp(result, -maximum, maximum));
                if (analyze) {
                    inputPeak = std::max(inputPeak, float(std::abs(input)));
                    outputPeak = std::max(outputPeak, std::abs(block.channel(c)[n]));
                    if (!c) { dryLeft = float(dry); processedLeft = float(std::clamp(processed, -maximum, maximum)); }
                }
            }
            previousDrive_ = d; previousShape_ = s; previousNorm_ = norm;
            if (analyze) {
                packet_.dry[packetOffset_] = dryLeft; packet_.processed[packetOffset_] = processedLeft;
                packet_.inputPeak = std::max(packet_.inputPeak, inputPeak); packet_.outputPeak = std::max(packet_.outputPeak, outputPeak);
                if (++packetOffset_ == analysisSize) {
                    packet_.sampleRate = sampleRate_; packet_.sequence = ++sequence_; mailbox->publish(packet_);
                    packetOffset_ = 0; packet_.inputPeak = packet_.outputPeak = 0;
                }
            }
        }
    }
private:
    struct Channel { Oversampling oversampler; std::array<double, latency> dry{}; unsigned position{}; };
    std::array<Channel, 2> channels_{};
    SmoothedValue drive_, shape_, mix_, output_, bypass_, delta_;
    double sampleRate_ = 48000, previousDrive_{}, previousShape_{}, previousNorm_{};
    std::uint32_t maxFrames_{};
    bool prepared_{}, initialized_{};
    AnalysisFrame packet_;
    std::size_t packetOffset_{};
    std::uint64_t sequence_{};
};
}
