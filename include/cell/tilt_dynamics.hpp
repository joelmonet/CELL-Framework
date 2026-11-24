// SPDX-License-Identifier: Apache-2.0
#pragma once
#include <algorithm>
#include <array>
#include <cmath>
#include <span>
namespace cell::dsp {
// Linked stereo or independent M/S peak envelopes. No allocation or lookahead.
class TiltDynamics {
 double rate_=48000;std::array<double,2> envelopes_{};
public:
 void prepare(double rate)noexcept{rate_=std::isfinite(rate)&&rate>0?rate:48000;reset();}
 void reset()noexcept{envelopes_.fill(0);}
 std::array<float,2> process(std::span<const float> left,std::span<const float> right,std::array<float,2> amounts,bool split,float threshold,float ratio,float attack,float release)noexcept{
  const double up=std::exp(-1/(rate_*std::max(.0001,double(attack)*.001))),down=std::exp(-1/(rate_*std::max(.0001,double(release)*.001)));
  for(std::size_t i=0;i<left.size();++i){const double l=std::isfinite(left[i])?left[i]:0,r=i<right.size()&&std::isfinite(right[i])?right[i]:l;
   const std::array<double,2> levels=split?std::array{std::abs((l+r)*.5),std::abs((l-r)*.5)}:std::array{std::max(std::abs(l),std::abs(r)),std::max(std::abs(l),std::abs(r))};
   for(unsigned c=0;c<2;++c){const double pole=levels[c]>envelopes_[c]?up:down;envelopes_[c]=levels[c]+(envelopes_[c]-levels[c])*pole;}
  }
  std::array<float,2> result{};for(unsigned c=0;c<2;++c){const double over=std::max(0.,20*std::log10(std::max(1e-12,envelopes_[c]))-threshold),movement=over*(1-1/std::max(1.,double(ratio)));result[c]=std::copysign(float(std::min(double(std::abs(amounts[c])),movement)),amounts[c]);}return result;
 }
};
}
