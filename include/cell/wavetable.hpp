// SPDX-License-Identifier: Apache-2.0
#pragma once
#include <cstddef>
#include <span>
#include <vector>
namespace cell::dsp {
// Original, immutable cycle bank. Import/build run on a control/worker thread;
// interpolated, band-limited playback is allocation-free and noexcept.
class Wavetable {
public:
 static constexpr unsigned cycleSize=2048,mipCount=11,maximumFrames=256;
 static Wavetable import(std::span<const std::byte> bytes,unsigned frameSamples=0);
 static Wavetable fromCycles(std::span<const float> samples,unsigned frameSamples);
 float read(float position,double phase,double frequency,double sampleRate)const noexcept;
 // Interoperable mono float32 WAV with explicit clm cycle-length metadata.
 std::vector<std::byte> waveFile()const;
 unsigned frames()const noexcept{return frames_;}
 const std::vector<float>& cycles()const noexcept{return cycles_;}
private:
 unsigned frames_{};std::vector<float> data_,cycles_;
};
}
