#pragma once
#include <array>
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <numbers>

namespace cell::dsp {

// Symmetric 65-tap half-band FIR. Alternate coefficients are exactly zero;
// interpolation's even phase is a delayed copy. Coefficients are made off-thread.
class HalfbandCoefficients {
public:
    HalfbandCoefficients() {
        double total{};
        for (unsigned j = 0; j < 16; ++j) {
            const double index = 2 * j + 1, n = index - 32;
            const double angle = 2 * std::numbers::pi * index / 64;
            const double window = .35875 - .48829 * std::cos(angle) + .14128 * std::cos(2 * angle) - .01168 * std::cos(3 * angle);
            odd[j] = std::sin(.5 * std::numbers::pi * n) / (std::numbers::pi * n) * window;
            total += 2 * odd[j];
        }
        // Preserve the half-band center coefficient at exactly 0.5 and set DC
        // gain to one. The other half of the kernel is obtained by symmetry.
        for (auto& value : odd) value *= .5 / total;
    }
    std::array<double, 16> odd{};
};

class HalfbandUp {
public:
    void reset() noexcept { history_.fill(0); next_ = 0; }
    std::array<double, 2> process(double input, const HalfbandCoefficients& h) noexcept {
        history_[next_] = input; next_ = (next_ + 1) & 31;
        double odd{};
        for (unsigned j = 0; j < 16; ++j) odd += (delay(j) + delay(31 - j)) * (2 * h.odd[j]);
        return {delay(16), odd};
    }
private:
    double delay(unsigned n) const noexcept { return history_[(next_ + 31 - n) & 31]; }
    std::array<double, 32> history_{};
    unsigned next_{};
};

class HalfbandDown {
public:
    void reset() noexcept { history_.fill(0); next_ = 0; }
    double process(double even, double odd, const HalfbandCoefficients& h) noexcept {
        push(even);
        double output = .5 * delay(32);
        for (unsigned j = 0; j < 16; ++j) output += (delay(2 * j + 1) + delay(63 - 2 * j)) * h.odd[j];
        push(odd);
        return output;
    }
private:
    void push(double input) noexcept { history_[next_] = input; next_ = (next_ + 1) & 63; }
    double delay(unsigned n) const noexcept { return history_[(next_ + 63 - n) & 63]; }
    std::array<double, 64> history_{};
    unsigned next_{};
};

// 2x..64x prepared, bounded-memory cascade. A callback receives each oversampled
// point in time order. No intermediate block allocation or host-block assumption.
template<unsigned Stages>
class Oversampler {
    static_assert(Stages >= 1 && Stages <= 6);
public:
    static constexpr unsigned factor = 1u << Stages;
    static constexpr unsigned latency = 64 - 64 / factor;
    static constexpr unsigned tail = 2 * latency;
    void reset() noexcept { for (auto& s : up_) s.reset(); for (auto& s : down_) s.reset(); }
    template<class Function> double process(double input, Function&& function) noexcept {
        return stage<0>(input, function);
    }
private:
    template<unsigned Index, class Function> double stage(double input, Function& function) noexcept {
        auto pair = up_[Index].process(input, coefficients_);
        if constexpr (Index + 1 == Stages) { pair[0] = function(pair[0]); pair[1] = function(pair[1]); }
        else { pair[0] = stage<Index + 1>(pair[0], function); pair[1] = stage<Index + 1>(pair[1], function); }
        return down_[Index].process(pair[0], pair[1], coefficients_);
    }
    HalfbandCoefficients coefficients_;
    std::array<HalfbandUp, Stages> up_{};
    std::array<HalfbandDown, Stages> down_{};
};

// High quality boundary filters. The wider input interpolator preserves the
// audible band; the final decimator gives up the last few percent of bandwidth
// to reject harmonics immediately above the host Nyquist frequency. A compact
// half-band alone has only ~6 dB rejection right at that boundary.
class BoundaryUp {
public:
    BoundaryUp() {
        double total{};
        for (unsigned j = 0; j < 32; ++j) {
            const auto index = double(2 * j + 1), n = index - 64;
            const auto a = 2 * std::numbers::pi * index / 128;
            const auto window = .35875 - .48829 * std::cos(a) + .14128 * std::cos(2 * a) - .01168 * std::cos(3 * a);
            coefficients_[j] = std::sin(.5 * std::numbers::pi * n) / (std::numbers::pi * n) * window; total += 2 * coefficients_[j];
        }
        for (auto& v : coefficients_) v *= .5 / total;
    }
    void reset() noexcept { history_.fill(0); next_ = 0; }
    std::array<double, 2> process(double input) noexcept {
        history_[next_] = input; next_ = (next_ + 1) & 63;
        double odd{}; for (unsigned j = 0; j < 32; ++j) odd += (delay(j) + delay(63 - j)) * (2 * coefficients_[j]);
        return {delay(32), odd};
    }
private:
    double delay(unsigned n) const noexcept { return history_[(next_ + 63 - n) & 63]; }
    std::array<double, 32> coefficients_{};
    std::array<double, 64> history_{}; unsigned next_{};
};

class BoundaryDown {
public:
    BoundaryDown() {
        double total{};
        for (unsigned j = 0; j <= 192; ++j) {
            const auto n = double(j) - 192, a = 2 * std::numbers::pi * j / 384;
            const auto window = .35875 - .48829 * std::cos(a) + .14128 * std::cos(2 * a) - .01168 * std::cos(3 * a);
            coefficients_[j] = (j == 192 ? .47 : std::sin(.47 * std::numbers::pi * n) / (std::numbers::pi * n)) * window;
            total += coefficients_[j] * (j == 192 ? 1 : 2);
        }
        for (auto& v : coefficients_) v /= total;
    }
    void reset() noexcept { history_.fill(0); next_ = 0; }
    double process(double even, double odd) noexcept {
        push(even); double output = coefficients_[192] * delay(192);
        for (unsigned j = 0; j < 192; ++j) output += coefficients_[j] * (delay(j) + delay(384 - j));
        push(odd); return output;
    }
private:
    void push(double input) noexcept { history_[next_] = input; next_ = (next_ + 1) & 511; }
    double delay(unsigned n) const noexcept { return history_[(next_ + 511 - n) & 511]; }
    std::array<double, 193> coefficients_{};
    std::array<double, 512> history_{}; unsigned next_{};
};

template<unsigned Stages>
class GuardedOversampler {
    static_assert(Stages >= 2 && Stages <= 6);
public:
    static constexpr unsigned factor = 1u << Stages;
    static constexpr unsigned latency = 128 + Oversampler<Stages - 1>::latency / 2;
    static constexpr unsigned tail = 2 * latency;
    void reset() noexcept { up_.reset(); inner_.reset(); down_.reset(); }
    template<class Function> double process(double input, Function&& function) noexcept {
        auto pair = up_.process(input); pair[0] = inner_.process(pair[0], function); pair[1] = inner_.process(pair[1], function);
        return down_.process(pair[0], pair[1]);
    }
private:
    BoundaryUp up_; Oversampler<Stages - 1> inner_; BoundaryDown down_;
};
}
