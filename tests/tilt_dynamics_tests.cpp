// SPDX-License-Identifier: Apache-2.0
#include <cell/tilt_dynamics.hpp>
#include "allocation_watch.hpp"
#include <iostream>
#include <stdexcept>
using cell::dsp::TiltDynamics;
void check(bool x,const char* why){if(!x)throw std::runtime_error(why);}
int main(){try{
 for(double rate:{44100.,48000.,96000.}){
  std::array<float,256> left,right;left.fill(.25f);right.fill(.25f);TiltDynamics split;split.prepare(rate);std::array<float,2> result;
  cell_test::allocations=cell_test::deletions=0;cell_test::audio=true;for(unsigned n=0;n<100;++n)result=split.process(left,right,{12,-12},true,-30,2,2,80);cell_test::audio=false;
  check(!cell_test::allocations&&!cell_test::deletions,"Tilt detector allocated on audio thread");check(result[0]>8.8&&result[0]<9.1&&result[1]==0,"Mid envelope/channel isolation failed");
  split.reset();right.fill(-.25f);for(unsigned n=0;n<100;++n)result=split.process(left,right,{12,-12},true,-30,2,2,80);check(result[0]==0&&result[1]<-8.8&&result[1]>-9.1,"Side envelope/channel isolation failed");
  split.reset();for(unsigned n=0;n<100;++n)result=split.process(left,right,{3,-4},false,-30,2,2,80);check(result[0]==3&&result[1]==-4,"Linked stereo ignored signed amount limits");
  result=split.process(left,right,{3,-4},false,-30,1,2,80);check(result[0]==0&&result[1]==0,"Unity ratio applied dynamics");
  left.fill(0);right.fill(0);for(unsigned n=0;n<1000;++n)result=split.process(left,right,{3,-4},false,-30,2,2,80);check(result[0]==0&&result[1]==0,"Envelope did not release into silence");
 }
 std::cout<<"PASS zero-allocation tilt envelopes: independent M/S, linked stereo, signed bounds, unity ratio and release at three sample rates\n";
}catch(const std::exception& e){cell_test::audio=false;std::cerr<<e.what()<<'\n';return 1;}}
