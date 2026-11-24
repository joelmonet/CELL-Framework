// SPDX-License-Identifier: Apache-2.0
#pragma once
#include <array>
#include <algorithm>
#include <cmath>
#include <span>
#include <stdexcept>
namespace cell::dsp {
enum class ResonanceFocus {upperMids,lowMids};
inline constexpr float resonanceCutLimit(ResonanceFocus focus)noexcept{return focus==ResonanceFocus::upperMids?6.f:8.f;}
// Weighting changes where correction is wanted, never the gentle-mode cap.
// Amount has already been applied by the detector; ratio and boundary apply here.
inline float weightedResonanceCut(float detected,float boundary,float weight,float depth,ResonanceFocus focus,float ratioSlope=1)noexcept{
 if(!std::isfinite(detected)||!std::isfinite(boundary)||!std::isfinite(weight)||!std::isfinite(depth)||!std::isfinite(ratioSlope))return 0;
 const float limit=std::min(std::max(0.f,depth),resonanceCutLimit(focus));
 const double wanted=double(std::max(0.f,detected))*std::clamp(boundary,0.f,1.f)*std::max(0.f,weight)*std::clamp(ratioSlope,0.f,1.f);
 return float(std::min(double(limit),wanted));
}
// Fixed storage, analysis only. Geometry/focus weights are prepared off audio.
// Persistent local prominence identifies resonances; flux protects attacks and
// a neighbouring-bin kernel avoids isolated mask discontinuities.
template<unsigned Bins> class ResonanceMask {
 static_assert(Bins>=8);
 struct Neighbours {unsigned lo{},hi{},excludeLo{},excludeHi{},count=1;};
 std::array<float,Bins> previous_{},persistence_{},raw_{},mask_{},upperWeight_{},lowerWeight_{};
 std::array<Neighbours,Bins> neighbours_{};std::array<double,Bins+1> prefix_{};
 float rise_{},fall_{};bool prepared_{};
 static float control(float value,float maximum)noexcept{return std::isfinite(value)?std::clamp(value,0.f,maximum):0;}
 float harmonic(unsigned center)const noexcept{float result=0;const unsigned lo=center?center-1:0,hi=std::min(center+1,Bins-1);for(unsigned n=lo;n<=hi;++n)result=std::max(result,previous_[n]);return result;}
public:
 void prepare(double rate,unsigned hop){
  if(!std::isfinite(rate)||rate<8000||rate>768000||!hop)throw std::invalid_argument("Invalid resonance detector preparation");
  rise_=float(std::exp(-double(hop)/(rate*.030)));fall_=float(std::exp(-double(hop)/(rate*.100)));
  for(unsigned k=0;k<Bins;++k){const double hz=k*rate/double(2*(Bins-1));const unsigned radius=std::max(6u,unsigned(k*.12));auto& w=neighbours_[k];w.lo=k>radius?k-radius:0;w.hi=std::min(Bins-1,k+radius);w.excludeLo=std::max(w.lo,k>2?k-2:0);w.excludeHi=std::min(w.hi,k+2);w.count=std::max(1u,w.hi-w.lo+1-(w.excludeHi-w.excludeLo+1));
   const auto weight=[&](double center,double width){const double octaves=std::log2(std::max(20.,hz)/center);return float(std::exp(-.5*octaves*octaves/(width*width)));};upperWeight_[k]=hz>=1200&&hz<=10000?weight(3500,.85):0;lowerWeight_[k]=hz>=90&&hz<=1000?weight(300,.8):0;
  }prepared_=true;reset();
 }
 void reset()noexcept{previous_={};persistence_={};raw_={};mask_={};}
 void analyze(std::span<const float> magnitude,ResonanceFocus focus,float amount,float depth,float selectivity)noexcept{
  if(!prepared_||magnitude.size()!=Bins){mask_={};return;}
  double positive=0,total=0;float maximum=0;prefix_[0]=0;
  for(unsigned k=0;k<Bins;++k){const float value=std::isfinite(magnitude[k])?std::max(0.f,magnitude[k]):0;positive+=std::max(0.f,value-previous_[k]);total+=value;previous_[k]=value;maximum=std::max(maximum,value);prefix_[k+1]=prefix_[k]+value;}
  amount=control(amount,1);depth=control(depth,36);selectivity=control(selectivity,1);
  if(amount==0||depth==0||maximum<=1.e-12f){for(auto& p:persistence_)p*=fall_;raw_={};mask_={};return;}
  const float protection=float(std::clamp(1.-3.*positive/std::max(1.e-12,total),.08,1.)),threshold=6-3*selectivity,limit=std::min(depth,resonanceCutLimit(focus));
  const bool harsh=focus==ResonanceFocus::upperMids;const auto& weights=harsh?upperWeight_:lowerWeight_;const float floor=std::max(1.e-12f,maximum*1.e-5f);
  for(unsigned k=0;k<Bins;++k){float weight=weights[k];if(weight==0||previous_[k]<floor){persistence_[k]*=fall_;raw_[k]=0;continue;}
   const auto w=neighbours_[k];const double baseline=std::max(1.e-30,(prefix_[w.hi+1]-prefix_[w.lo]-(prefix_[w.excludeHi+1]-prefix_[w.excludeLo]))/w.count);
   const float prominence=float(20*std::log10(std::max(1.e-30,double(previous_[k]))/baseline)),excess=std::max(0.f,prominence-threshold),pole=excess>persistence_[k]?rise_:fall_;persistence_[k]=pole*persistence_[k]+(1-pole)*excess;
   // Adjacent bins account for an off-bin harmonic's leakage. Prefer leaving
   // a pitched fundamental alone to thinning the body's coherent harmonics.
   if(!harsh&&k>1&&k*3<Bins&&harmonic(k*2)>.12f*previous_[k]&&harmonic(k*3)>.06f*previous_[k])weight*=.25f;
   raw_[k]=limit*std::tanh(persistence_[k]*.35f/std::max(1.f,depth))*amount*weight*protection;
  }
  for(unsigned k=0;k<Bins;++k)mask_[k]=weights[k]>0?.25f*raw_[k?k-1:0]+.5f*raw_[k]+.25f*raw_[std::min(k+1,Bins-1)]:0;
 }
 float operator[](unsigned k)const noexcept{return k<Bins?mask_[k]:0;}
};
}
