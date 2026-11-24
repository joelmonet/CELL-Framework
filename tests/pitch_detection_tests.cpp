// SPDX-License-Identifier: Apache-2.0
#include <cell/pitch_detection.hpp>
#include "allocation_watch.hpp"
#include <iostream>
#include <memory>
#include <numbers>
#include <stdexcept>
using namespace cell;
void check(bool ok,const char* message){if(!ok)throw std::runtime_error(message);}
int main(){try{
 const auto callbackAllocations=cell_test::allocations,callbackDeletions=cell_test::deletions;
 auto detector=std::make_unique<pitch::Detector>();detector->prepare(48000);check(!detector->configure({pitch::Mode::mono,0,1000,-54,false}),"Invalid pitch range accepted");
 for(auto mode:{pitch::Mode::mono,pitch::Mode::poly}){
  check(detector->configure({mode,55,1800,-54,false}),"Valid pitch range rejected");detector->reset();std::uint64_t first=0;
  const auto allocations=cell_test::allocations,deletions=cell_test::deletions;
  for(unsigned n=0;n<24000;++n){const float sample=float(.2*std::sin(2*std::numbers::pi*440*n/48000));cell_test::audio=true;const bool updated=detector->push(sample);cell_test::audio=false;if(updated&&detector->result().count&&!first)first=n+1;}
  check(cell_test::allocations==allocations&&cell_test::deletions==deletions,"Pitch evidence allocated on audio callback");const auto& r=detector->result();check(r.count==1&&std::abs(1200*std::log2(r.voices[0].hz/440))<3,"Prepared pitch evidence lost pure440Hz");check(first&&first<48000*.05,"Pure440Hz evidence exceeded50ms");
  for(unsigned n=0;n<12000;++n)detector->push(0);check(!detector->result().count,"Silent pitch evidence did not clear");
 }
 check(detector->configure({pitch::Mode::poly,55,1800,-54,false}),"Poly attack configuration rejected");detector->reset();
 for(unsigned n=0;n<24000;++n){double sample=0;if(n>=2400)for(double hz:{261.625565,329.627557,391.995436})for(unsigned h=1;h<=5;++h)sample+=.04/h*std::sin(2*std::numbers::pi*hz*h*n/48000);cell_test::audio=true;const bool changed=detector->push(float(int(sample*32767))/32767.f);cell_test::audio=false;if(changed)for(unsigned i=0;i<detector->result().count;++i){const auto key=std::lround(69+12*std::log2(detector->result().voices[i].hz/440.));check(key==60||key==64||key==67,"A delayed chord attack became a false high pitch");}}

 for(double sampleRate:{44100.,48000.,96000.}){detector->prepare(sampleRate);check(detector->configure({pitch::Mode::poly,55,1800,-54,false}),"Harmonic rate configuration rejected");
 for(double hz:{55.,82.406889,110.,220.,440.})for(unsigned timbre=0;timbre<3;++timbre){detector->reset();unsigned frames=0;for(unsigned n=0;n<unsigned(sampleRate*.5);++n){double sample=0;for(unsigned h=1;h<=48&&hz*h<6000;++h){const double amplitude=timbre==0?1./h:timbre==1?(h==1?.12:1./std::sqrt(h)):(h==1?.2:(h==2||h==3||h==7||h==13||h==17?.6:0));sample+=amplitude*std::sin(2*std::numbers::pi*hz*h*n/sampleRate);}cell_test::audio=true;const bool changed=detector->push(float(sample*.08));cell_test::audio=false;if(changed&&detector->result().count){++frames;check(detector->result().count==1&&std::lround(69+12*std::log2(detector->result().voices[0].hz/440.))==std::lround(69+12*std::log2(hz/440.)),"Bright harmonic source emitted extra high notes");}}check(frames>8,"Harmonic rejection lost the source fundamental");}
 }
 detector->prepare(48000);check(detector->configure({pitch::Mode::poly,55,1800,-54,false}),"Noise configuration rejected");
 for(unsigned seed=1;seed<=8;++seed){detector->reset();std::uint32_t random=seed;for(unsigned n=0;n<24000;++n){random=random*1664525u+1013904223u;const float sample=float((double(random)/4294967296.-.5)*.15);cell_test::audio=true;const bool changed=detector->push(sample);cell_test::audio=false;if(changed)check(!detector->result().count,"Broadband noise was interpreted as harmonic notes");}}
 check(cell_test::allocations==callbackAllocations&&cell_test::deletions==callbackDeletions,"Cross-rate pitch/harmonic/noise processing allocated or reclaimed callback memory");
 std::cout<<"PASS shared causal mono/poly pitch evidence, range validation, frequency precision, silence and no audio allocation/reclamation\n";
}catch(const std::exception& e){cell_test::audio=false;std::cerr<<e.what()<<'\n';return 1;}}
