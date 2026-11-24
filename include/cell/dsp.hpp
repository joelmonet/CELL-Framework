#pragma once

#include <cell/audio.hpp>
#include <complex>
#include <numbers>

namespace cell::dsp {

class SmoothedValue {
public:
    void prepare(double sampleRate, double seconds) {
        if(!std::isfinite(sampleRate)||sampleRate<=0||!std::isfinite(seconds)||seconds<0||
           !std::isfinite(sampleRate*seconds)||sampleRate*seconds>double(UINT32_MAX))
            throw std::invalid_argument("invalid smoothing time");
        ramp_ = std::max<std::uint32_t>(1, static_cast<std::uint32_t>(std::max(0.0, seconds) * sampleRate));
        reset(current_);
    }
    void reset(float value) noexcept { current_ = target_ = value; remaining_ = 0; step_ = 0; }
    void target(float value) noexcept {
        if (value == target_) return;
        target_ = value; remaining_ = ramp_; step_ = (target_ - current_) / float(ramp_);
    }
    float next() noexcept {
        if (remaining_ && --remaining_ == 0) current_ = target_;
        else if (remaining_) current_ += step_;
        return current_;
    }
    bool smoothing() const noexcept { return remaining_ != 0; }
    float current() const noexcept { return current_; }
private:
    float current_ = 1, target_ = 1, step_{};
    std::uint32_t ramp_ = 1, remaining_{};
};

class DelayLine {
public:
    void prepare(std::uint32_t maximumDelay) {
        if(maximumDelay>(1u<<24))throw std::invalid_argument("delay capacity too large");
        samples_.assign(std::size_t(maximumDelay) + 2, 0.0f); write_ = 0;
    }
    void reset() noexcept { std::fill(samples_.begin(), samples_.end(), 0); write_ = 0; }
    void push(float sample) noexcept { if(samples_.empty())return;samples_[write_] = sample; write_ = (write_ + 1) % samples_.size(); }
    float read(float delay) const noexcept {
        if (samples_.empty() || !std::isfinite(delay)) return 0;
        delay = std::clamp(delay, 0.0f, float(samples_.size() - 2));
        double position = double(write_) - 1.0 - delay;
        while (position < 0) position += double(samples_.size());
        if(position>=double(samples_.size()))position=0;
        const auto a = std::size_t(position), b = (a + 1) % samples_.size();
        return samples_[a] + float(position - double(a)) * (samples_[b] - samples_[a]);
    }
private:
    std::vector<float> samples_;
    std::size_t write_{};
};

struct BiquadCoefficients {
    double b0 = 1, b1{}, b2{}, a1{}, a2{};
    static BiquadCoefficients lowpass(double sampleRate, double frequency, double q = 0.7071067811865476) {
        if (!std::isfinite(sampleRate) || sampleRate <= 0 || !std::isfinite(frequency) ||
            frequency <= 0 || frequency >= sampleRate * .5 || !std::isfinite(q) || q <= 0)
            throw std::invalid_argument("invalid biquad design");
        const double w = 2.0 * std::numbers::pi * frequency / sampleRate;
        const double c = std::cos(w), alpha = std::sin(w) / (2 * q), a0 = 1 + alpha;
        return {(1-c)*.5/a0, (1-c)/a0, (1-c)*.5/a0, -2*c/a0, (1-alpha)/a0};
    }
};

class Biquad {
public:
    void coefficients(BiquadCoefficients c) noexcept { c_ = c; }
    void reset() noexcept { z1_ = z2_ = 0; }
    float process(float sample) noexcept {
        const double y = c_.b0 * sample + z1_;
        z1_ = c_.b1 * sample - c_.a1 * y + z2_;
        z2_ = c_.b2 * sample - c_.a2 * y;
        return static_cast<float>(y);
    }
private:
    BiquadCoefficients c_{};
    double z1_{}, z2_{};
};

// Prepared radix-2 transform. Forward is unscaled; inverse divides by N.
class FFT {
public:
    explicit FFT(std::size_t size) : size_(size) {
        if (size < 2 || (size & (size - 1))) throw std::invalid_argument("FFT size must be a power of two");
        roots_.resize(size/2);
        for (std::size_t i=0; i<size/2; ++i)
            roots_[i] = std::polar(1.0f, float(-2 * std::numbers::pi * i / size));
    }
    void transform(std::span<std::complex<float>> data, bool inverse = false) const noexcept {
        if (data.size() != size_) return;
        for (std::size_t i=1,j=0;i<size_;++i) {
            std::size_t bit=size_>>1;
            for (;j&bit;bit>>=1) j^=bit;
            j^=bit;
            if(i<j) std::swap(data[i],data[j]);
        }
        for(std::size_t length=2;;length<<=1) {
            const auto stride=size_/length;
            for(std::size_t offset=0;offset<size_;offset+=length)
                for(std::size_t j=0;j<length/2;++j) {
                    const auto root=inverse?std::conj(roots_[j*stride]):roots_[j*stride];
                    const auto even=data[offset+j], odd=data[offset+j+length/2]*root;
                    data[offset+j]=even+odd; data[offset+j+length/2]=even-odd;
                }
            if(length==size_)break;
        }
        if(inverse) for(auto& value:data) value/=float(size_);
    }
private:
    std::size_t size_;
    std::vector<std::complex<float>> roots_;
};

// Prepared real-input spectrum. Scratch and twiddles are allocated once; repeated
// transforms do not allocate and preserve the unscaled-forward convention.
class RealFFT {
    FFT fft_;std::vector<std::complex<float>>work_;
  public:
    explicit RealFFT(std::size_t size):fft_(size),work_(size){}
    std::size_t size()const noexcept{return work_.size();}
    // Interleaved real/imaginary positive spectrum occupies N+2 floats in a
    // 2*N buffer. Inverse reconstructs the Hermitian half and divides by N.
    void forward(std::span<float> inOut, bool positiveOnly=true) {
        if(inOut.size()<size()*2)throw std::invalid_argument("Real FFT spectrum storage is too short");
        for(std::size_t i=0;i<size();++i)work_[i]={inOut[i],0};
        fft_.transform(work_);
        const auto count=positiveOnly?size()/2+1:size();
        for(std::size_t i=0;i<count;++i){inOut[2*i]=work_[i].real();inOut[2*i+1]=work_[i].imag();}
    }
    void inverse(std::span<float> inOut) {
        if(inOut.size()<size()*2)throw std::invalid_argument("Real FFT spectrum storage is too short");
        work_[0]={inOut[0],0};work_[size()/2]={inOut[size()],0};
        for(std::size_t i=1;i<size()/2;++i){work_[i]={inOut[2*i],inOut[2*i+1]};work_[size()-i]=std::conj(work_[i]);}
        fft_.transform(work_,true);
        for(std::size_t i=0;i<size();++i)inOut[i]=work_[i].real();
    }
    void magnitude(std::span<float>inOut,bool positiveOnly=true){
        if(inOut.size()<size())throw std::invalid_argument("Real FFT input is shorter than its prepared size");
        for(std::size_t i=0;i<size();++i)work_[i]={inOut[i],0};
        fft_.transform(work_);const auto count=positiveOnly?size()/2+1:size();
        for(std::size_t i=0;i<count;++i)inOut[i]=std::abs(work_[i]);
    }
};

} // namespace cell::dsp
