// SPDX-License-Identifier: Apache-2.0
#pragma once
#include <cell/synthesis.hpp>
#include <array>
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <numbers>
namespace cell::dsp {
inline constexpr unsigned percussionModelCount=23;
struct PercussionSettings {
 unsigned model{};float tune=55,keyTrack=0,sweep=24,sweepTime=.035f,decay=.5f,noise=.2f,noiseDecay=.12f,snap=.3f,snapTime=.0025f,body=.4f,metal=.2f,drive=0,stereo=.15f;
 bool operator==(const PercussionSettings&)const=default;
};
struct PercussionProfile {
 std::array<double,6> ratios{1,1.593,2.136,2.296,2.653,2.918};
 double tonal=1,noise=1,low=1000,high=10000,decayScale=1,noiseScale=1,pitchScale=1,attack=0;
};
inline PercussionProfile percussionProfile(unsigned model)noexcept{
 PercussionProfile p;
 switch(model){
 case 6:p.ratios={1,1.48,2.11,2.65,3.38,4.07};p.decayScale=.45;p.noise=.25;p.low=2200;break; // rim
 case 7:p.ratios={1,1.49,2.71,3.83,5.13,6.41};p.noise=.08;p.low=1800;p.pitchScale=.08;break; // cowbell
 case 8:p.ratios={1,2.72,5.43,8.93,13.34,18.64};p.decayScale=.35;p.noise=.06;p.pitchScale=0;break; // clave
 case 9:p.tonal=.03;p.low=3500;p.high=14000;p.noiseScale=.8;p.attack=.0015;break; // shaker
 case 10:p.ratios={1,1.342,1.729,2.397,3.05,4.11};p.tonal=.6;p.low=4200;p.high=15000;p.pitchScale=.05;break; // ride
 case 11:p.ratios={1,2.127,3.51,5.76,8.01,10.42};p.decayScale=.55;p.noise=.12;p.pitchScale=.1;break; // block
 case 12:p.ratios={1,1.73,2.41,3.17,4.31,5.83};p.tonal=.18;p.low=5000;p.high=16000;p.pitchScale=0;p.attack=.001;break; // jingles
 case 13:p.ratios={1,1.502,2.05,2.39,2.82,3.32};p.low=600;p.high=6500;p.noise=.5;p.decayScale=1.25;break; // taiko
 case 14:p.ratios={1,2,3,4,5,6};p.noise=.12;p.low=800;p.pitchScale=.65;break; // sub kick
 case 15:p.ratios={1,1.997,3.04,4.11,5.23,7.07};p.low=1800;p.high=12000;p.pitchScale=1.25;p.decayScale=.8;break; // punch
 case 16:p.ratios={1,2.13,3.87,5.16,7.01,9.33};p.low=2200;p.noise=.2;p.pitchScale=.85;break; // FM kick
 case 17:p.ratios={1,1.593,2.136,2.296,2.653,2.918};p.low=1700;p.high=11000;p.decayScale=.7;p.attack=.0007;break; // acoustic snare
 case 18:p.tonal=.08;p.low=1200;p.high=8000;p.attack=.006;p.pitchScale=.1;break; // brush
 case 19:p.ratios={1,1.8333,2.71,4.13,5.12,6.39};p.low=2800;p.high=13500;p.pitchScale=.3;break; // electronic snare
 case 20:p.ratios={1,1.47,2.09,2.56,3.11,3.77};p.noise=.3;p.low=700;p.high=7000;p.decayScale=1.2;p.pitchScale=.7;break;
 case 21:p.ratios={1,1.61,2.19,2.74,3.42,4.18};p.noise=.4;p.low=1500;p.high=9000;p.decayScale=.8;p.pitchScale=.85;break;
 case 22:p.ratios={1,1.593,2.136,2.296,2.653,2.918};p.noise=.3;p.low=1000;p.high=8000;p.decayScale=1.15;p.pitchScale=.55;break;
 default:break;
 }return p;
}
// Original tone/noise/transient percussion model. No audio buffers or samples.
class PercussionVoice {
 std::array<double,6> phase_{};PercussionSettings previous_{};PercussionProfile profile_{};double rate_{},pitch_=0,tone_=0,noise_=0,snap_=0,pitchPole_{},tonePole_{},noisePole_{},snapPole_{},lowPole_{},highPole_{},noiseLow_[2]{},noiseHigh_[2]{},driveGain_=1,driveScale_=1,attack_=1,attackPole_=1;std::uint64_t age_{};std::uint32_t random_=1;bool trigger_=false;
 float random()noexcept{random_=random_*1664525u+1013904223u;return float(random_>>8)/8388608.f-1;}
 void configure(double rate,const PercussionSettings& s)noexcept{rate_=rate;previous_=s;profile_=percussionProfile(s.model);attackPole_=profile_.attack>0?1-std::exp(-1/(profile_.attack*rate)):1;driveGain_=std::pow(10.,std::clamp(double(s.drive),0.,24.)/20);driveScale_=1/std::sqrt(driveGain_);pitchPole_=std::exp(-1/(std::max(.001f,s.sweepTime)*rate));tonePole_=std::exp(-6.907755278982137/(std::max(.005,double(s.decay)*profile_.decayScale)*rate));noisePole_=std::exp(-6.907755278982137/(std::max(.002,double(s.noiseDecay)*profile_.noiseScale)*rate));snapPole_=std::exp(-6.907755278982137/(std::max(.0001f,s.snapTime)*rate));const double low=s.model>=6?profile_.low:s.model==4?6000:s.model==3?2500:s.model==5?700:1000,high=s.model>=6?profile_.high:s.model==4?16000:s.model==5?6500:10000;lowPole_=1-std::exp(-2*std::numbers::pi*std::min(low,rate*.4)/rate);highPole_=1-std::exp(-2*std::numbers::pi*std::min(high,rate*.43)/rate);}
public:
 void reset()noexcept{phase_={};pitch_=tone_=noise_=snap_=0;age_=0;noiseLow_[0]=noiseLow_[1]=noiseHigh_[0]=noiseHigh_[1]=0;trigger_=false;attack_=0;}
 void noteOn(std::uint32_t seed=1)noexcept{reset();random_=seed?seed:1;pitch_=tone_=noise_=snap_=1;trigger_=true;}
 bool active()const noexcept{return trigger_||tone_+noise_+snap_>1.e-6;}
 std::array<float,2> next(double fundamental,double rate,const PercussionSettings& s)noexcept{
  if(!active()||!std::isfinite(fundamental)||!std::isfinite(rate)||rate<8000)return {};
  if(trigger_||rate_!=rate||s.model!=previous_.model||s.sweepTime!=previous_.sweepTime||s.decay!=previous_.decay||s.noiseDecay!=previous_.noiseDecay||s.snapTime!=previous_.snapTime||s.drive!=previous_.drive){configure(rate,s);trigger_=false;}
  const double frequency=std::clamp(fundamental*std::exp2(std::clamp(double(s.sweep),0.,72.)*pitch_*(s.model>=6?profile_.pitchScale:1)/12),1.,rate*.4);
  constexpr std::array<double,6> metalRatios{1,1.342,1.729,2.397,3.05,4.11};constexpr std::array<double,6> drumRatios{1,1.593,2.136,2.296,2.653,2.918};double body=0,metal=0;
  for(unsigned n=0;n<phase_.size();++n){const double ratio=s.model>=6?profile_.ratios[n]:s.model==3||s.model==4?metalRatios[n]:drumRatios[n],hz=frequency*ratio,step=std::min(.45,hz/rate);if(hz<rate*.43){const double sinus=std::sin(2*std::numbers::pi*phase_[n]);double partial=sinus;if(s.model==16&&n==0)partial=std::sin(2*std::numbers::pi*phase_[0]+std::sin(2*std::numbers::pi*phase_[1])*pitch_*(1+double(s.body)*7));double weight=n?double(s.body)/(n+2):1;if(s.model==14&&n)weight*=.05;if(s.model==7)weight=n<2?1.:weight*.1;if(s.model==8&&n>1)weight*=.05;if(s.model==20&&n>2)weight*=.3;if(s.model==21&&n>0)weight*=1.4;if(s.model==22&&n>0)weight*=.85;body+=partial*weight;metal+=analogOscillator(phase_[n],step,1)/phase_.size();}phase_[n]+=step;phase_[n]-=std::floor(phase_[n]);}
  const double milliseconds=1000.*age_/rate;double clap=1;if(s.model==5){clap=0;for(double onset:{0.,12.,24.})if(milliseconds>=onset)clap+=std::exp(-(milliseconds-onset)/3.5);clap+=double(s.body)*.2*noise_;}
  attack_+=(1-attack_)*attackPole_;
  std::array<float,2> output{};const float common=random();for(unsigned c=0;c<2;++c){const double white=common+(random()-common)*std::clamp(double(s.stereo),0.,1.);noiseLow_[c]+=lowPole_*(white-noiseLow_[c]);noiseHigh_[c]+=highPole_*(white-noiseHigh_[c]);const double filtered=noiseHigh_[c]-noiseLow_[c];const double toneMix=s.model>=6?profile_.tonal:s.model==4?.12:s.model==5?.04:1,noiseMix=s.model>=6?profile_.noise:s.model==0?.25:s.model==2?.2:1;double tone=body,grain=1;if(s.model==7)tone=metal*2.2;if(s.model==10)tone=body*.75+metal*.35;if(s.model==12)tone=metal*1.4;if(s.model==9||s.model==12){const double period=s.model==9?.008:.013,phase=std::fmod(double(age_)/(rate*period)+c*.23,1.);grain=.2+.8*std::sin(std::numbers::pi*phase)*std::sin(std::numbers::pi*phase);}if(s.model==15)tone+=std::tanh(body*3)*snap_*.5;if(s.model==19)tone=body*.7+metal*.45;double value=(tone*toneMix+metal*std::clamp(double(s.metal),0.,1.))*tone_*.55+filtered*std::clamp(double(s.noise),0.,1.)*noise_*noiseMix*clap*(s.model>=6?attack_*grain:1)+white*std::clamp(double(s.snap),0.,1.)*snap_*.25;if(driveGain_>1)value=std::tanh(value*driveGain_)*driveScale_;output[c]=float(value);}
  pitch_*=pitchPole_;tone_*=tonePole_;noise_*=noisePole_;snap_*=snapPole_;++age_;return output;
 }
};
}
