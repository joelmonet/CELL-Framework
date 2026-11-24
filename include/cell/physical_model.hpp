// SPDX-License-Identifier: Apache-2.0
#pragma once
#include <array>
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <numbers>
namespace cell::dsp {
inline constexpr unsigned physicalModelCount=11;
inline constexpr bool physicalModelUsesPressure(unsigned model)noexcept{return model==1||model==4||model==9||model==10;}
inline constexpr bool physicalModelUsesBow(unsigned model)noexcept{return model==1||model==9||model==10;}
// Original modal physical model. String/tube eigenfrequencies, stiff-bar
// partials and rectangular-membrane modes share a bounded resonator bank.
struct PhysicalModelSettings {
 unsigned model{};float exciter=.6f,position=.23f,decay=2.4f,damping=.45f,stiffness=.1f,body=.35f,bow=.4f,pressure=.35f,noise=.08f,stereo=.55f;
 bool operator==(const PhysicalModelSettings&)const=default;
};
class PhysicalModelVoice {
 struct Mode{double real{},imag{},cosine=1,sine{},radius{};float left{},right{},force{};};
 std::array<Mode,24> modes_{};PhysicalModelSettings previous_{};double previousHz_=-1,previousRate_{},lastForce_{},windPhase_{};unsigned countdown_{};std::uint32_t random_=1;bool trigger_=false,running_=false;
 float random()noexcept{random_=random_*1664525u+1013904223u;return float(random_>>8)/8388608.f-1;}
 void configure(double fundamental,double rate,const PhysicalModelSettings& s)noexcept{
  constexpr std::array<double,8> bar{1,2.756,5.404,8.933,13.344,18.64,24.88,32.13};
  constexpr std::array<double,8> bell{1,2,2.378,3,4.071,5.431,6.812,8.215},wood{1,3.932,9.538,16.688,25.568,36.352,49.04,63.63};
  for(unsigned n=0;n<modes_.size();++n){auto& mode=modes_[n];const double harmonic=n+1;double ratio=harmonic;
   if(s.model==2)ratio=n<bar.size()?bar[n]:std::pow((n+1.5)/1.5,2);
   else if(s.model==3){const double x=n%4+1,y=n/4+1;ratio=std::sqrt((x*x+y*y)/2);}
   else if(s.model==4||s.model==9)ratio=2*n+1;
   else if(s.model==5)ratio=n<bell.size()?bell[n]:8.215+1.53*(n-7)+.037*(n-7)*(n-7);
   else if(s.model==6)ratio=n<wood.size()?wood[n]:std::pow((n+.5)/1.5,2)*2.25;
   else if(s.model==7)ratio=n==0?1:std::pow((n+.5)*std::numbers::pi/1.8751040687,2);
   else if(s.model==8)ratio=harmonic*(1+std::clamp(double(s.body),0.,1.)*.0017*harmonic);
   else if(s.model==10)ratio=harmonic;
   ratio*=std::sqrt(1+std::clamp(double(s.stiffness),0.,1.)*.003*ratio*ratio);
   const double hz=std::clamp(fundamental,1.,rate*.4)*ratio,angle=2*std::numbers::pi*std::min(hz,rate*.45)/rate;
   const double material=s.model==6?1+ratio*.15:s.model==7?1+ratio*.06:s.model==5?1+ratio*.018:1;
   const double decay=std::max(.02,double(s.decay))/(material*(1+std::clamp(double(s.damping),0.,1.)*.22*std::pow(ratio,1.2)));
   mode.cosine=std::cos(angle);mode.sine=std::sin(angle);mode.radius=std::exp(-6.907755278982137/(decay*rate));
   const double brightness=std::exp(-ratio*(1-std::clamp(double(s.exciter),0.,1.))*.2)/std::sqrt(ratio),body=1+std::clamp(double(s.body),0.,1.)*.8*std::exp(-std::pow((ratio-3)/2,2));
   const double position=std::clamp(double(s.position),.01,.99),width=std::clamp(double(s.stereo),0.,1.)*.065;
   mode.left=hz<rate*.43?float(brightness*body*std::sin(std::numbers::pi*harmonic*std::clamp(position-width,.01,.99))*.19):0;
   mode.right=hz<rate*.43?float(brightness*body*std::sin(std::numbers::pi*harmonic*std::clamp(position+width,.01,.99))*.19):0;
   mode.force=s.model>=5&&hz>=rate*.43?0:float(std::sin(std::numbers::pi*harmonic*position)/std::sqrt(ratio));
  }previous_=s;previousHz_=fundamental;previousRate_=rate;
 }
public:
 void reset()noexcept{for(auto& mode:modes_){mode.real=mode.imag=0;}previousHz_=-1;countdown_=0;lastForce_=windPhase_=0;trigger_=false;running_=false;}
 void noteOn(std::uint32_t seed=1)noexcept{reset();random_=seed?seed:1;trigger_=true;running_=true;}
 std::array<float,2> next(double fundamental,double rate,const PhysicalModelSettings& settings,bool held=true)noexcept{
  if(!running_||!std::isfinite(fundamental)||!std::isfinite(rate)||rate<8000)return {};
  if(trigger_||!countdown_){if(trigger_||fundamental!=previousHz_||rate!=previousRate_||settings!=previous_)configure(fundamental,rate,settings);countdown_=16;}--countdown_;
  if(trigger_){for(auto& mode:modes_){mode.real=physicalModelUsesPressure(settings.model)?0:mode.force*(1+random()*std::clamp(settings.noise,0.f,1.f));mode.imag=0;}trigger_=false;}
  double velocity=0,displacement=0;for(const auto& mode:modes_){velocity+=mode.imag*mode.force*.035;displacement+=mode.real*mode.force*.035;}
  double force=0;if(held&&(settings.model==1||settings.model==4)){const double pressure=std::clamp(double(settings.pressure),0.,1.);force=settings.model==1?(std::tanh((std::clamp(double(settings.bow),0.,1.)*.025-velocity)*(10+pressure*70))+random()*std::clamp(double(settings.noise),0.,1.)*.1)*pressure*.025:random()*std::clamp(double(settings.noise),0.,1.)*pressure*.06;}
  if(held&&(settings.model==9||settings.model==10)){
   windPhase_+=std::clamp(fundamental/rate,0.,.4);windPhase_-=std::floor(windPhase_);const double pressure=std::clamp(double(settings.pressure),0.,1.),embouchure=std::clamp(double(settings.bow),0.,1.),oscillator=std::sin(2*std::numbers::pi*windPhase_);
   // Driven nonlinear reed/lip excitation into acoustic modal resonances.
   // This is not a full autonomous waveguide or a calibrated wind instrument.
   const double reed=std::tanh((oscillator-displacement*4)*(1+embouchure*5))*(.6+.4*oscillator),lip=std::tanh(oscillator*(1+pressure*5)+velocity*(1+embouchure*4));
   force=(settings.model==9?reed:lip)*pressure*.025+random()*std::clamp(double(settings.noise),0.,1.)*pressure*.006;
  }
  if(settings.model==8)force=std::tanh(velocity*20)*std::abs(std::tanh(displacement*4))*std::clamp(double(settings.body),0.,1.)*.015;
  const double excitation=force-lastForce_;lastForce_=force;std::array<float,2> output{};
  for(auto& mode:modes_){const double real=mode.radius*(mode.real*mode.cosine-mode.imag*mode.sine)+excitation*mode.force,imag=mode.radius*(mode.real*mode.sine+mode.imag*mode.cosine);mode.real=std::clamp(real,-8.,8.);mode.imag=std::clamp(imag,-8.,8.);output[0]+=float(mode.real)*mode.left;output[1]+=float(mode.real)*mode.right;}
  return output;
 }
};
}
