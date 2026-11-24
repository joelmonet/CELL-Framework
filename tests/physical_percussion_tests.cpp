// SPDX-License-Identifier: Apache-2.0
// Deferred: no compilation or runtime execution authorized for this revision.
#include <cell/physical_model.hpp>
#include <cell/percussion.hpp>
#include <cell/synthesis.hpp>
#include <cell/dsp.hpp>
#include "allocation_watch.hpp"
#include <iostream>
#include <stdexcept>
using namespace cell;
namespace {
void check(bool ok,const char* reason){if(!ok)throw std::runtime_error(reason);}
void physical(){
 for(double rate:{44100.,48000.,96000.})for(unsigned model=0;model<dsp::physicalModelCount;++model){
  dsp::PhysicalModelSettings settings;settings.model=model;settings.decay=.12f;settings.noise=.7f;settings.pressure=.8f;
  dsp::PhysicalModelVoice first,second;first.noteOn(123);second.noteOn(123);double energy=0,early=0,late=0;
  cell_test::allocations=cell_test::deletions=0;cell_test::audio=true;
  bool finite=true,identical=true;for(unsigned n=0;n<unsigned(rate*.7);++n){const auto a=first.next(220,rate,settings,n<unsigned(rate*.15)),b=second.next(220,rate,settings,n<unsigned(rate*.15));for(unsigned c=0;c<2;++c){finite&=std::isfinite(a[c]);identical&=a[c]==b[c];const double e=double(a[c])*a[c];energy+=e;if(n<rate*.05)early+=e;if(n>rate*.65)late+=e;}}
  cell_test::audio=false;check(finite&&identical,"Modal model was nonfinite or seed-dependent rendering was not repeatable");check(energy>1.e-8,"Physical model was silent");check(late<early*.02,"Physical model failed to damp after excitation stopped");check(!cell_test::allocations&&!cell_test::deletions,"Physical rendering allocated or freed callback memory");
  first.reset();const auto silence=first.next(220,rate,settings,false);check(silence[0]==0&&silence[1]==0,"Physical reset retained ringing state");
 }
}
void percussion(){
 for(double rate:{44100.,48000.,96000.})for(unsigned model=0;model<dsp::percussionModelCount;++model){
  dsp::PercussionSettings settings;settings.model=model;settings.decay=.05f;settings.noiseDecay=.04f;settings.drive=12;settings.noise=.8f;
  dsp::PercussionVoice voice,copy;voice.noteOn(321);copy.noteOn(321);double energy=0,early=0,late=0;bool finite=true,identical=true;
  cell_test::allocations=cell_test::deletions=0;cell_test::audio=true;
  for(unsigned n=0;n<unsigned(rate*.4);++n){const auto a=voice.next(110,rate,settings),b=copy.next(110,rate,settings);for(unsigned c=0;c<2;++c){finite&=std::isfinite(a[c]);identical&=a[c]==b[c];const double e=double(a[c])*a[c];energy+=e;if(n<rate*.05)early+=e;if(n>rate*.35)late+=e;}}
  cell_test::audio=false;check(finite&&identical&&energy>1.e-8,"Percussion model was silent, nonfinite or nondeterministic");check(late<early*.001&&!voice.active(),"One-shot percussion failed to decay to idle");check(!cell_test::allocations&&!cell_test::deletions,"Percussion rendering allocated or freed callback memory");voice.reset();check(!voice.active(),"Percussion reset did not stop the one-shot");
 }
}
template<unsigned Count,class Voice,class Settings>void distinct(){
 std::array<std::array<float,2048>,Count> audio{};
 for(unsigned model=0;model<Count;++model){Voice voice;Settings settings;settings.model=model;settings.noise=.5f;voice.noteOn(111);for(unsigned n=0;n<audio[model].size();++n)audio[model][n]=voice.next(220,48000,settings)[0];}
 for(unsigned a=0;a<Count;++a)for(unsigned b=a+1;b<Count;++b){double difference=0,energy=0;for(unsigned n=0;n<audio[a].size();++n){const double x=audio[a][n],y=audio[b][n];difference+=(x-y)*(x-y);energy+=x*x+y*y;}check(energy>1.e-10&&difference>energy*1.e-5,"Two named models produced the same audition waveform");}
}
void fades(){
 dsp::TransitionFade fade;fade.reset(96);float previous=1;for(unsigned n=0;n<96;++n){const auto value=fade.next();check(value<=previous&&value>=0,"Transition fade overshot");previous=value;}check(!fade.remaining()&&fade.next()==0,"Fade duration changed with block boundaries");
 fade.reset(0);check(fade.next()==0,"Zero-length fade did not complete");
 dsp::SmoothedValue ramp;ramp.prepare(48000,.003);ramp.reset(0);ramp.target(1);for(unsigned n=0;n<144;++n){if(n%17==0)ramp.target(1);ramp.next();}check(!ramp.smoothing()&&ramp.current()==1,"Repeated block targets restarted the control ramp");
}
}
int main(){try{physical();percussion();distinct<dsp::physicalModelCount,dsp::PhysicalModelVoice,dsp::PhysicalModelSettings>();distinct<dsp::percussionModelCount,dsp::PercussionVoice,dsp::PercussionSettings>();fades();std::cout<<"PASS modal/percussion model decay, deterministic seeds, callback ownership and sample-counted fades\n";}catch(const std::exception& e){cell_test::audio=false;std::cerr<<e.what()<<'\n';return 1;}}
