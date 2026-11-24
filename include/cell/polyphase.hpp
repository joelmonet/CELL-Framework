#pragma once
#include <cell/audio.hpp>
#include <cell/halfband_profiles.hpp>
#include <array>
#include <vector>
#include <cmath>
#include <stdexcept>
#include <complex>
#include <numbers>

namespace cell::dsp {
// Low-latency half-band interpolation/decimation with two complementary
// all-pass branches. Each branch runs at the lower rate. All storage belongs
// to the prepared instance; processing is bounded by maximumFrames.
class PolyphaseOversampler {
 struct Branch {
  std::array<float,4> memory{};
  float process(float input,const detail::HalfbandFilter& filter,unsigned parity)noexcept{for(unsigned j=parity,k=0;j<filter.count;j+=2,++k){const float a=filter.alpha[j],output=a*input+memory[k];memory[k]=input-a*output;input=output;}return input;}
 };
 struct Channel {Branch upEven,upOdd,downEven,downOdd;float previousOdd{},delayX{},delayY{};};
 struct Stage {std::array<Channel,16> channels;AudioBuffer<float> samples;detail::HalfbandStageProfile profile;};
 // A small-frequency phase measurement describes this minimum-phase filter's
 // effective delay. The published all-pass transfer function avoids designing
 // or convolving a long direct-form polynomial during preparation.
 static double phaseDelay(const detail::HalfbandFilter& filter){
  constexpr double omega=2*std::numbers::pi*.0001;const auto z=std::polar(1.,-omega),z2=z*z;std::complex<double> even=1,odd=z;
  for(unsigned j=0;j<filter.count;++j){const double a=filter.alpha[j];(j&1?odd:even)*=(a+z2)/(1.+a*z2);}
  return -std::arg((even+odd)*.5)/omega;
 }
public:
 void prepare(unsigned channels,unsigned stages,unsigned maximumFrames,bool integralLatency=true,bool maximumQuality=true){
  if(!channels||channels>16||!stages||stages>6||!maximumFrames||maximumFrames>1048576)throw std::invalid_argument("invalid polyphase oversampling dimensions");
  channelCount_=channels;maximum_=maximumFrames;stages_.resize(stages);float delay=0,scale=2;
  for(unsigned j=0;j<stages;++j){auto& stage=stages_[j];stage.profile=(maximumQuality?detail::maximumQuality:detail::economyQuality)[j];stage.samples.resize(channels,maximumFrames*(2u<<j));delay+=float(phaseDelay(stage.profile.up)+phaseDelay(stage.profile.down))/scale;scale*=2;}
  float fraction=0;if(integralLatency){fraction=1-(delay-std::floor(delay));if(fraction>=1)fraction=0;else if(fraction<.618f)fraction+=1;}
  compensate_=fraction>0;compensation_=(1-fraction)/(1+fraction);latency_=double(delay+fraction);reset();
 }
 void reset()noexcept{for(auto& stage:stages_)for(auto& channel:stage.channels)channel={};frames_=0;}
 double latency()const noexcept{return latency_;}
 AudioBlock<float> up(AudioBlock<float> input)noexcept{
  if(stages_.empty()||!input.channelCount||input.channelCount>channelCount_||input.frames>maximum_)return {};
  frames_=input.frames;activeChannels_=input.channelCount;for(auto& stage:stages_){auto output=stage.samples.block().slice(0,input.frames*2);output.channelCount=activeChannels_;for(unsigned c=0;c<activeChannels_;++c){auto& state=stage.channels[c];for(unsigned n=0;n<input.frames;++n){const float x=input.channel(c)[n];output.channel(c)[2*n]=state.upEven.process(x,stage.profile.up,0);output.channel(c)[2*n+1]=state.upOdd.process(x,stage.profile.up,1);}}input=output;}return input;
 }
 void down(AudioBlock<float> output)noexcept{
  if(stages_.empty()||output.channelCount!=activeChannels_||output.frames!=frames_)return;
  for(std::size_t j=stages_.size();j-->0;){const unsigned length=frames_*(1u<<j);auto input=stages_[j].samples.block().slice(0,length*2);auto target=j?stages_[j-1].samples.block().slice(0,length):output;
   for(unsigned c=0;c<activeChannels_;++c){auto& state=stages_[j].channels[c];for(unsigned n=0;n<length;++n){const auto a=state.downEven.process(input.channel(c)[2*n],stages_[j].profile.down,0),b=state.downOdd.process(state.previousOdd,stages_[j].profile.down,1);state.previousOdd=input.channel(c)[2*n+1];float value=.5f*(a+b);if(j==0&&compensate_){const float next=state.delayX+compensation_*(value-state.delayY);state.delayX=value;state.delayY=next;value=next;}target.channel(c)[n]=value;}}
  }
 }
private:std::vector<Stage> stages_;unsigned channelCount_{},maximum_{},frames_{},activeChannels_{};double latency_{};float compensation_{};bool compensate_{};
};
}
