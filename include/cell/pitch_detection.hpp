// SPDX-License-Identifier: Apache-2.0
#pragma once
#include <cell/dsp.hpp>
#include <array>
#include <cstdint>
namespace cell::pitch {
enum class Mode { mono, poly };
struct Config {
    Mode mode=Mode::mono;
    double minimumHz=55,maximumHz=1800,gateDb=-54;
    bool economy{};
    bool operator==(const Config&)const=default;
};
struct Voice {double hz{},confidence{},amplitude{};};
struct Result {
    std::array<Voice,8> voices{};unsigned count{};
    double rms{},windowMilliseconds{};
    std::uint64_t sample{};
};
// Causal pitch evidence only. Audio is not delayed or changed. Storage and FFT
// twiddles belong to the control-thread construction/preparation path; push()
// neither allocates nor invokes workers. A result's sample is its decision time,
// not an estimated historical onset. Lower notes require more observed cycles.
class Detector {
public:
    void prepare(double sampleRate);
    bool configure(const Config&)noexcept;
    void reset()noexcept;
    bool push(float)noexcept;
    const Result& result()const noexcept{return result_;}
    double analysisRate()const noexcept{return rate_;}
private:
    static constexpr unsigned historySize=2048,fftSize=2048,peakLimit=96;
    struct Peak {double hz{},amplitude{};bool used{};};
    struct Candidate {double hz{},score{},confidence{};std::array<unsigned char,peakLimit> matches{},harmonics{};unsigned count{};int first=-1;};
    void analyseMono()noexcept;
    void analysePoly()noexcept;
    double recent(unsigned back)const noexcept{return history_[(write_+historySize-1-back)%historySize];}
    double distance(unsigned lag,unsigned compare)const noexcept;
    Config config_{};Result result_{};bool prepared_{},polyLong_{};
    double sampleRate_=48000,rate_=12000,pole_{},average_{},gateAmplitude_=.0019952623149688795;
    std::array<double,4> lowpass_{};
    unsigned decimation_=4,decimated_{},write_{},seen_{},hop_{},signalSeen_{},quietSamples_{};
    std::uint64_t clock_{};double monoPitch_{};unsigned monoMisses_{};
    std::array<float,historySize> history_{};
    std::array<double,1025> difference_{};
    dsp::FFT fft_{fftSize};std::array<std::complex<float>,fftSize> spectrum_{};
    std::array<double,fftSize/2+1> magnitude_{};
    std::array<std::array<float,historySize>,4> windows_{};
    std::array<Peak,peakLimit> peaks_{};
    std::array<Candidate,peakLimit> candidates_{};
    std::array<double,183> harmonicWeights_{};
};
}
