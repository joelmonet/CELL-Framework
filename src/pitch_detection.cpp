// SPDX-License-Identifier: Apache-2.0
#include <cell/pitch_detection.hpp>
#include <algorithm>
#include <cmath>
#include <numbers>
namespace cell::pitch {
void Detector::prepare(double sr){
 if(!std::isfinite(sr)||sr<8000||sr>384000)throw std::invalid_argument("Pitch detector sample rate");
 sampleRate_=sr;decimation_=std::max(1u,unsigned(std::lround(sr/12000.)));rate_=sr/decimation_;
 pole_=1-std::exp(-2*std::numbers::pi*std::min(3500.,rate_*.33)/sr);
 for(unsigned w=0;w<4;++w){const unsigned length=256u<<w;for(unsigned i=0;i<length;++i)windows_[w][i]=float(.5-.5*std::cos(2*std::numbers::pi*i/(length-1)));}
 for(unsigned h=1;h<harmonicWeights_.size();++h)harmonicWeights_[h]=1/std::sqrt(double(h));
 prepared_=true;reset();
}
bool Detector::configure(const Config& c)noexcept{
 if((c.mode!=Mode::mono&&c.mode!=Mode::poly)||!std::isfinite(c.minimumHz)||!std::isfinite(c.maximumHz)||!std::isfinite(c.gateDb)||c.minimumHz<22||c.maximumHz>4000||c.minimumHz>=c.maximumHz||c.gateDb< -100||c.gateDb>0)return false;
 if(c.mode!=config_.mode||c.minimumHz!=config_.minimumHz||c.maximumHz!=config_.maximumHz||c.economy!=config_.economy)reset();if(c.gateDb!=config_.gateDb)gateAmplitude_=std::pow(10.,c.gateDb/20);config_=c;return true;
}
void Detector::reset()noexcept{history_.fill(0);lowpass_.fill(0);average_=0;decimated_=write_=seen_=hop_=signalSeen_=quietSamples_=0;clock_=0;monoPitch_=0;monoMisses_=0;polyLong_=false;result_={};}
bool Detector::push(float sample)noexcept{
 if(!prepared_)return false;++clock_;double x=std::isfinite(sample)?std::clamp(sample,-32.f,32.f):0;
 for(auto& low:lowpass_){low+=pole_*(x-low);x=low;}average_+=x;
 if(++decimated_<decimation_)return false;
 const auto observed=float(average_/decimation_);
 // Count actual signal history, not preceding silence. A full circular FFT
 // buffer can otherwise turn a short, abruptly started chord into apparent
 // high notes before its fundamentals have enough observed cycles.
 if(std::abs(observed)>=gateAmplitude_){quietSamples_=0;if(!signalSeen_)signalSeen_=1;}
 else{quietSamples_=std::min(64u,quietSamples_+1);if(quietSamples_==64){signalSeen_=0;polyLong_=false;}}
 if(signalSeen_)signalSeen_=std::min(historySize,signalSeen_+1);
 history_[write_]=observed;average_=0;decimated_=0;write_=(write_+1)%historySize;seen_=std::min(historySize,seen_+1);
 const unsigned hop=config_.mode==Mode::mono?(config_.economy?64:monoPitch_>200?16:32):(config_.economy?256:128);
 if(++hop_<hop||seen_<128)return false;hop_=0;result_.count=0;result_.sample=clock_-1;
 if(config_.mode==Mode::mono){analyseMono();if(result_.count){monoPitch_=result_.voices[0].hz;monoMisses_=0;}else{monoMisses_=std::min(2u,monoMisses_+1);if(monoMisses_==2)monoPitch_=0;}}else analysePoly();return true;
}
double Detector::distance(unsigned lag,unsigned compare)const noexcept{
 double sum=0;for(unsigned j=0;j<compare;++j){const auto d=recent(j)-recent(j+lag);sum+=d*d;}return sum/compare;
}
void Detector::analyseMono()noexcept{
 const unsigned available=std::min(seen_,historySize),minLag=std::max(2u,unsigned(rate_/config_.maximumHz));
 const unsigned fullMaximum=unsigned(std::ceil(rate_/config_.minimumHz));
 // An established period bounds useful candidates. Two missed observations
 // reopen the complete range, so a real interval jump is not held at the old
 // pitch. High notes can update more frequently while doing less total work.
 const unsigned predicted=monoPitch_>0?unsigned(std::ceil(rate_/monoPitch_*1.8))+2:fullMaximum;
 const unsigned maxLag=std::min({1022u,fullMaximum,predicted,unsigned((available-1)/2.5)});
 if(maxLag<=minLag+1)return;
 const unsigned powerCount=std::min(available,128u);double mean=0,power=0;
 for(unsigned j=0;j<powerCount;++j){const double x=recent(j);mean+=x;power+=x*x;}
 result_.rms=std::sqrt(std::max(0.,power/powerCount-(mean/powerCount)*(mean/powerCount)));
 if(result_.rms<gateAmplitude_)return;
 double fresh=0;for(unsigned j=0;j<64;++j){const double x=recent(j);fresh+=x*x;}
 if(fresh/64<result_.rms*result_.rms*.025)return;
 double cumulative=0;difference_[0]=1;
 for(unsigned lag=1;lag<=maxLag;++lag){const unsigned compare=std::min(available-lag,std::max(64u,unsigned(std::ceil(lag*1.5))));const double d=distance(lag,compare);cumulative+=d;difference_[lag]=cumulative>1.e-20?d*lag/cumulative:1;}
 int best=-1;for(unsigned lag=minLag;lag<maxLag;++lag)if(difference_[lag]<.13){while(lag+1<=maxLag&&difference_[lag+1]<difference_[lag])++lag;best=int(lag);break;}
 if(best<0)return;double refined=best;const unsigned compare=std::min(available-unsigned(best)-1,std::max(64u,unsigned(std::ceil(best*1.5))));
 if(best>1&&unsigned(best)<maxLag){const double a=distance(unsigned(best)-1,compare),b=distance(unsigned(best),compare),c=distance(unsigned(best)+1,compare),den=a-2*b+c;if(den>1.e-15)refined+=std::clamp(.5*(a-c)/den,-.5,.5);}
 const double hz=rate_/refined;if(hz<config_.minimumHz||hz>config_.maximumHz)return;
 const unsigned envelopeWindow=std::min(available,std::max(128u,unsigned(std::ceil(refined*2))));double envelope=0,offset=0;for(unsigned j=0;j<envelopeWindow;++j){const double x=recent(j);envelope+=x*x;offset+=x;}
 const double amplitude=std::sqrt(std::max(0.,envelope/envelopeWindow-(offset/envelopeWindow)*(offset/envelopeWindow)));
 result_.voices[0]={hz,std::clamp(1-difference_[unsigned(best)],0.,1.),amplitude};result_.count=1;result_.windowMilliseconds=(best+compare)*1000/rate_;
}
void Detector::analysePoly()noexcept{
 unsigned length=512,which=1;
 if(config_.minimumHz<45||config_.economy||polyLong_){length=1024;which=2;}
 // Start high-note evidence before the full window is available. Each decision
 // describes only observed audio; no event is shifted backwards in time.
 const unsigned desiredLength=length;
 const auto observed=std::min(seen_,signalSeen_);
 if(observed<length){if(observed<256)return;length=observed>=512?512:256;which=length==512?1:0;}
 spectrum_.fill({});double mean=0,power=0;for(unsigned i=0;i<length;++i)mean+=recent(i);mean/=length;
 for(unsigned i=0;i<length;++i){const double v=recent(length-1-i)-mean;power+=v*v;spectrum_[i]={float(v*windows_[which][i]),0};}
 result_.rms=std::sqrt(power/length);result_.windowMilliseconds=length*1000/rate_;
 if(result_.rms<gateAmplitude_)return;
 double recentPower=0;for(unsigned i=0;i<64;++i){const double x=recent(i);recentPower+=x*x;}
 // A cut-off tail is not a fresh high-pitched attack. The long FFT window
 // still contains the preceding note after the recent input has gone quiet.
 if(recentPower/64<power/length*.025)return;
 fft_.transform(spectrum_);for(unsigned i=0;i<=fftSize/2;++i)magnitude_[i]=std::abs(spectrum_[i]);
 const unsigned first=std::max(2u,unsigned(40*fftSize/rate_)),last=std::min(fftSize/2-1,unsigned(4000*fftSize/rate_));
 double maximum=0,total=0,logs=0;for(unsigned i=first;i<=last;++i){const double m=std::max(1.e-12,magnitude_[i]);maximum=std::max(maximum,m);total+=m;logs+=std::log(m);}
 const unsigned bins=last-first+1;if(std::exp(logs/bins)/(total/bins)>.65)return;
 unsigned peakCount=0;
 for(unsigned i=2;i<last&&peakCount<peakLimit;++i){const double y=magnitude_[i];if(y<maximum*.035||y<=magnitude_[i-1]||y<magnitude_[i+1])continue;
  const double a=std::log(std::max(1.e-12,magnitude_[i-1])),b=std::log(y),c=std::log(std::max(1.e-12,magnitude_[i+1])),den=a-2*b+c;
  const double hz=(i+(std::abs(den)>1.e-12?std::clamp(.5*(a-c)/den,-.5,.5):0))*rate_/fftSize;
  if(hz>=config_.minimumHz*.97&&hz<4000)peaks_[peakCount++]={hz,y,false};
 }
 unsigned count=0;
 for(unsigned p=0;p<peakCount&&count<peakLimit;++p){const auto& peak=peaks_[p];if(peak.hz>config_.maximumHz)continue;auto& v=candidates_[count++];v={};v.hz=peak.hz;v.first=int(p);double estimate=0,weights=0;
  // Match observed peaks once, using their actual harmonic order. Sparse
  // spectra must not promote a 17th partial to the weight of a second partial.
  // The bound covers every harmonic below 4 kHz at the lowest 22 Hz range.
  const unsigned passes=peak.hz<100?2:1;
  for(unsigned pass=0;pass<passes;++pass){const double base=pass?v.hz:peak.hz;unsigned previous=0;v.count=0;weights=estimate=0;
  for(unsigned q=p;q<peakCount;++q){const auto& partial=peaks_[q];const unsigned h=unsigned(std::lround(partial.hz/base));if(!h||h>=harmonicWeights_.size()||(passes==2&&!pass&&h>8))continue;
   const double error=std::abs(partial.hz-base*h),tolerance=std::max(rate_/length*.62,partial.hz*.009);if(error>=tolerance)continue;
   if(h==previous){const auto prior=unsigned(v.matches[v.count-1]);if(std::abs(peaks_[prior].hz-base*h)<=error)continue;const double weight=peaks_[prior].amplitude*harmonicWeights_[h];weights-=weight;estimate-=peaks_[prior].hz/h*weight;--v.count;}
   const double weight=partial.amplitude*harmonicWeights_[h];v.matches[v.count]=static_cast<unsigned char>(q);v.harmonics[v.count++]=static_cast<unsigned char>(h);previous=h;weights+=weight;estimate+=partial.hz/h*weight;
  }
  if(weights>0)v.hz=estimate/weights;
  }
  v.score=weights;
  v.score*=1+.22*std::min(5u,v.count-1);v.confidence=std::min(.99,.76+.055*v.count);
 }
 double firstExplanation=0;
 for(unsigned voice=0;voice<result_.voices.size();++voice){int best=-1;double score=maximum*.10;
  for(unsigned i=0;i<count;++i){const auto& v=candidates_[i];if(peaks_[unsigned(v.first)].used)continue;double available=0;unsigned matches=0;for(unsigned h=0;h<v.count;++h)if(!peaks_[unsigned(v.matches[h])].used){available+=peaks_[unsigned(v.matches[h])].amplitude*harmonicWeights_[v.harmonics[h]];++matches;}available*=1+.22*std::min(5u,matches?matches-1:0);if(available>score){score=available;best=int(i);}}
  if(best<0)break;const auto& chosen=candidates_[unsigned(best)];if(!voice){double total=0,explained=0;for(unsigned i=0;i<peakCount;++i)total+=peaks_[i].amplitude;for(unsigned h=0;h<chosen.count;++h)explained+=peaks_[unsigned(chosen.matches[h])].amplitude;firstExplanation=total>0?explained/total:0;}result_.voices[result_.count++]={chosen.hz,chosen.confidence,2*peaks_[unsigned(chosen.first)].amplitude/length};for(unsigned h=0;h<chosen.count;++h)peaks_[unsigned(chosen.matches[h])].used=true;
 }
 // Closely packed low partials with a weak fundamental need more observed
 // cycles. At 44.1 kHz the weak fundamental can disappear from the short
 // window entirely, leaving its second/third partial as the first peak.
 // Escalate only ambiguous low spectra; clean higher notes keep the
 // short causal onset path. The longer window is released at silence/reset.
 if(length==512&&peakCount&&peaks_[0].hz<180&&result_.count&&(result_.count>1||result_.voices[0].hz>peaks_[0].hz*1.5)){polyLong_=true;result_.count=0;return;}
 // Short initial windows may establish a clean single harmonic source. A
 // chord needs the complete window so a temporarily merged fundamental cannot
 // cause its visible upper partial to be emitted as an extra MIDI note.
 if(length<desiredLength&&(result_.count!=1||firstExplanation<.85||result_.voices[0].hz*length/rate_<3))result_.count=0;
 std::sort(result_.voices.begin(),result_.voices.begin()+result_.count,[](auto a,auto b){return a.hz<b.hz;});
}
}
