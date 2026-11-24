// SPDX-License-Identifier: Apache-2.0
#pragma once
#include <algorithm>
#include <array>
#include <cmath>
#include <numbers>
#include <cstdint>
namespace cell::dsp {
inline double polyBlep(double phase,double step)noexcept{step=std::clamp(step,1.e-9,.49);if(phase<step){const auto t=phase/step;return t+t-t*t-1;}if(phase>1-step){const auto t=(phase-1)/step;return t*t+t+t+1;}return 0;}
inline float analogOscillator(double phase,double step,float shape)noexcept{
 phase-=std::floor(phase);const double sine=std::sin(2*std::numbers::pi*phase),saw=2*phase-1-polyBlep(phase,step),pulse=(phase<.5?1.:-1.)+polyBlep(phase,step)-polyBlep(std::fmod(phase+.5,1.),step);shape=std::clamp(shape,0.f,1.f);return float(shape<.5?sine+(saw-sine)*shape*2:saw+(pulse-saw)*(shape*2-1));
}
// Sample-counted cubic fade. Endpoints have zero slope; copying carries the
// current transition state. No callbacks, storage ownership or allocation.
class TransitionFade {
 std::uint32_t remaining_{};float inverse_{};
public:
 void reset(std::uint32_t samples)noexcept{remaining_=samples;inverse_=samples?1.f/samples:0;}
 void clear()noexcept{remaining_=0;}
 float value()const noexcept{const float x=remaining_*inverse_;return x*x*(3-2*x);}
 float next()noexcept{const float result=value();if(remaining_)--remaining_;return result;}
 std::uint32_t remaining()const noexcept{return remaining_;}
};
class Portamento {
 double current_=60,start_=60,target_=60,progress_=1,increment_=1,exponent_=1;
public:
 void reset(double note)noexcept{current_=start_=target_=note;progress_=1;}
 void target(double note,double seconds,double rate,double curve,bool glide)noexcept{start_=current_;target_=note;progress_=glide&&seconds>0?0:1;increment_=seconds>0?1/std::max(1.,seconds*rate):1;exponent_=std::exp2(std::clamp(curve,-1.,1.)*3);if(progress_==1)current_=target_;}
 double next()noexcept{if(progress_<1){progress_=std::min(1.,progress_+increment_);const double t=exponent_==1?progress_:std::pow(progress_,exponent_);current_=start_+(target_-start_)*t;}return current_;}
};
// Original six-operator PM/FM graph. Descending edges use this sample; cycles
// and self feedback use the previous sample, keeping arbitrary graphs bounded.
struct FmOperator {float ratio=1,level=1,feedback=0,attack=.01f,decay=.4f,sustain=.5f,release=.25f;};
struct FmGraph {std::array<FmOperator,6> operators;std::array<std::array<float,6>,6> route{};std::array<float,6> carrier{1,0,0,0,0,0};};
inline void fmAlgorithm(FmGraph& graph,unsigned algorithm)noexcept{
 graph.route={};graph.carrier={};
 const auto edge=[&](unsigned from,unsigned to){graph.route[to][from]=1;};
 switch(algorithm){case 0:for(unsigned n=1;n<6;++n)edge(n,n-1);graph.carrier[0]=1;break;
 case 1:edge(1,0);edge(3,2);edge(5,4);graph.carrier={.58f,0,.58f,0,.58f,0};break;
 case 2:for(unsigned n=1;n<6;++n)edge(n,0);graph.carrier[0]=1;break;
 case 3:edge(2,1);edge(1,0);edge(5,4);edge(4,3);graph.carrier={.7f,0,0,.7f,0,0};break;
 case 4:for(unsigned n=1;n<6;++n)edge(5,n-1);graph.carrier={.45f,.45f,.45f,.45f,.45f,0};break;
 case 5:graph.carrier.fill(.4082483f);break;
 case 6:edge(1,0);edge(2,1);edge(0,2);edge(4,3);edge(5,4);edge(3,5);graph.carrier={.7f,0,0,.7f,0,0};break;
 default:edge(5,4);edge(4,3);edge(3,2);edge(2,1);edge(1,0);graph.route[5][0]=.15f;graph.carrier[0]=1;break;}
}
class FmVoice {
 std::array<double,6> phase_{};std::array<float,6> previous_{},envelope_{},releaseStep_{};std::array<unsigned char,6> stage_{};
public:
 void reset()noexcept{phase_={};previous_={};envelope_={};stage_={};}
 void noteOn(bool retrigger=true)noexcept{if(retrigger){envelope_={};stage_.fill(1);}else for(auto& s:stage_)if(!s||s==4)s=1;}
 void noteOff(const FmGraph& graph,double rate)noexcept{for(unsigned n=0;n<6;++n){stage_[n]=4;releaseStep_[n]=envelope_[n]/std::max(1.f,float(rate)*graph.operators[n].release);}}
 float next(double fundamental,double rate,const FmGraph& graph,float intensity=1)noexcept{
  std::array<float,6> next{};double sum=0;
  for(unsigned ri=6;ri-->0;){const auto& op=graph.operators[ri];auto& env=envelope_[ri];switch(stage_[ri]){case 1:env+=1/std::max(1.f,float(rate)*op.attack);if(env>=1){env=1;stage_[ri]=2;}break;case 2:env-= (1-op.sustain)/std::max(1.f,float(rate)*op.decay);if(env<=op.sustain){env=op.sustain;stage_[ri]=3;}break;case 4:env=std::max(0.f,env-releaseStep_[ri]);if(env==0)stage_[ri]=0;break;default:break;}
   double mod=op.feedback*previous_[ri];for(unsigned j=0;j<6;++j)mod+=graph.route[ri][j]*(j>ri?next[j]:previous_[j]);
   const double hz=std::clamp(fundamental*op.ratio,0.,rate*.4);const float index=float(std::min(8.,rate*.15/std::max(1.,hz)))*std::clamp(intensity,0.f,2.f);
   next[ri]=float(std::sin(2*std::numbers::pi*phase_[ri]+index*std::tanh(mod)))*op.level*env;
   phase_[ri]+=hz/rate;phase_[ri]-=std::floor(phase_[ri]);sum+=next[ri]*graph.carrier[ri];
  }previous_=next;return float(sum);
 }
};
}
