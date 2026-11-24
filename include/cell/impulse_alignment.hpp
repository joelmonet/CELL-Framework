// SPDX-License-Identifier: Apache-2.0
#pragma once
#include <algorithm>
#include <cmath>
#include <complex>
#include <numbers>
#include <span>
namespace cell::dsp {
struct ImpulseSpectrum {std::span<const std::complex<float>> bins;double peakAlignmentMs{};bool enabled{};};
struct ImpulseAlignment {double delayMs{},coherence{};bool invert{};};
// Control/worker-thread analysis. The caller owns all input/output memory.
// Search near peak alignment, then choose polarity by weighted coherence.
// This is an optional recommendation; no audio callback or filter is changed.
inline bool alignImpulseSpectra(std::span<const ImpulseSpectrum> sources,std::span<const double> hz,double sampleRate,std::span<ImpulseAlignment> result,unsigned reference=0)noexcept{
 if(sources.empty()||result.size()<sources.size()||reference>=sources.size()||!sources[reference].enabled||hz.empty()||!std::isfinite(sampleRate)||sampleRate<8000)return false;
 const auto& base=sources[reference];if(base.bins.size()!=hz.size())return false;
 for(unsigned channel=0;channel<sources.size();++channel){const auto& input=sources[channel];auto& out=result[channel];out={std::clamp(input.peakAlignmentMs,0.,20.),0,false};if(!input.enabled||input.bins.size()!=hz.size())continue;if(channel==reference){out.coherence=1;continue;}
  const int steps=std::max(1,int(sampleRate*.001));double best=-1;
  for(int step=-steps;step<=steps;++step){const double delay=std::clamp(input.peakAlignmentMs+1000.*step/sampleRate,0.,20.);double numerator=0,denominator=0;
   for(unsigned n=0;n<hz.size();++n){if(hz[n]<100||hz[n]>12000)continue;const auto a=std::complex<double>(base.bins[n]),b=std::complex<double>(input.bins[n]);const double weight=1/std::sqrt(hz[n]);const auto cross=a*std::conj(b)*std::polar(1.,2*std::numbers::pi*hz[n]*(delay-base.peakAlignmentMs)*.001);numerator+=cross.real()*weight;denominator+=std::abs(a)*std::abs(b)*weight;}
   const double signedScore=denominator>1.e-20?numerator/denominator:0,score=std::abs(signedScore);if(score>best){best=score;out={delay,score,signedScore<0};}
  }
 }return true;
}
}
