// SPDX-License-Identifier: Apache-2.0
// Deferred source tests. No compilation/runtime executed during the hold.
#include <cell/resonance_mask.hpp>
#include "allocation_watch.hpp"
#include <iostream>
#include <limits>
using namespace cell::dsp;
namespace {
void check(bool ok,const char* why){if(!ok)throw std::runtime_error(why);}
using Mask=ResonanceMask<513>;
float settle(Mask& mask,const std::array<float,513>& input,ResonanceFocus focus,unsigned bin){for(unsigned n=0;n<100;++n)mask.analyze(input,focus,1,12,.75);return mask[bin];}
void caps(){
 check(weightedResonanceCut(6,1,8,18,ResonanceFocus::upperMids)==6,"Graph boost multiplied the gentle harshness cap");check(weightedResonanceCut(8,1,8,18,ResonanceFocus::lowMids)==8,"Graph boost multiplied the gentle mud cap");check(weightedResonanceCut(6,1,8,2,ResonanceFocus::upperMids)==2,"User depth did not cap a weighted cut");check(weightedResonanceCut(6,1,1,12,ResonanceFocus::upperMids,0)==0,"Unity ratio still produced correction");check(weightedResonanceCut(6,0,8,12,ResonanceFocus::upperMids)==0,"Out-of-band graph weighting produced correction");check(weightedResonanceCut(std::numeric_limits<float>::quiet_NaN(),1,1,12,ResonanceFocus::lowMids)==0,"Nonfinite detector control escaped the mask");
}
void detector(){
 std::array<float,513> input;input.fill(1);constexpr unsigned peak=75;input[peak]=100;
 Mask mask,quiet;mask.prepare(48000,256);quiet.prepare(48000,256);mask.analyze(input,ResonanceFocus::upperMids,1,12,.75);const float transient=mask[peak];const auto stable=settle(mask,input,ResonanceFocus::upperMids,peak);check(stable>.1&&transient<stable*.3,"Detector did not prefer persistent resonances over the onset");
 auto lowLevel=input;for(auto& value:lowLevel)value*=.001f;const auto quietCut=settle(quiet,lowLevel,ResonanceFocus::upperMids,peak);check(std::abs(stable-quietCut)<1.e-4,"Input level changed a relative resonance decision");
 Mask mud,pitched;mud.prepare(48000,256);pitched.prepare(48000,256);std::array<float,513> body;body.fill(1);body[6]=100;auto harmonic=body;harmonic[12]=30;harmonic[18]=20;const auto unpitchedCut=settle(mud,body,ResonanceFocus::lowMids,6),pitchedCut=settle(pitched,harmonic,ResonanceFocus::lowMids,6);check(unpitchedCut>.1&&pitchedCut<unpitchedCut*.5,"Coherent low harmonics were mistaken for the same amount of mud");
 std::array<float,513> silence{};mask.analyze(silence,ResonanceFocus::upperMids,1,12,.75);for(unsigned n=0;n<513;++n)check(mask[n]==0,"Silent input retained a displayed cut");mask.analyze(input,ResonanceFocus::upperMids,0,12,.75);check(mask[peak]==0,"Zero amount did not clear the detector output");
 mask.analyze(input,ResonanceFocus::upperMids,1,12,.75);mask.analyze(std::span(input).first(2),ResonanceFocus::upperMids,1,12,.75);check(mask[peak]==0,"Malformed input retained a stale mask");
 input[4]=std::numeric_limits<float>::infinity();input[peak]=std::numeric_limits<float>::quiet_NaN();cell_test::allocations=cell_test::deletions=0;cell_test::audio=true;for(unsigned n=0;n<16;++n)mask.analyze(input,ResonanceFocus::upperMids,1,12,.75);cell_test::audio=false;for(unsigned n=0;n<513;++n)check(std::isfinite(mask[n])&&mask[n]>=0&&mask[n]<=6,"Nonfinite input or out-of-range detector output");check(!cell_test::allocations&&!cell_test::deletions,"Resonance analysis allocated or freed on audio");
 bool rejected=false;try{mask.prepare(0,256);}catch(const std::invalid_argument&){rejected=true;}check(rejected,"Invalid preparation was accepted");
}
}
int main(){try{caps();detector();std::cout<<"PASS gentle resonance bounds, persistence, harmonic/attack protection, level invariance and callback storage\n";}catch(const std::exception& e){cell_test::audio=false;std::cerr<<e.what()<<'\n';return 1;}}
