// SPDX-License-Identifier: Apache-2.0
// Deferred source: build/runtime checks resume only when authorized.
#include <cell/wavetable.hpp>
#include <cell/synthesis.hpp>
#include <cell/core.hpp>
#include <cell/json.hpp>
#include "allocation_watch.hpp"
#include <bit>
#include <iostream>
using namespace cell;
namespace {
void check(bool value,const char* message){if(!value)throw std::runtime_error(message);}
template<class F>void rejects(F&& function){bool rejected=false;try{function();}catch(const std::exception&){rejected=true;}check(rejected,"Unsupported or malformed wavetable was admitted");}
std::vector<float> sine(unsigned size,unsigned harmonic=1){std::vector<float> result(size);for(unsigned n=0;n<size;++n)result[n]=float(std::sin(2*std::numbers::pi*harmonic*n/size));return result;}
void roundtrip(){auto wave=dsp::Wavetable::fromCycles(sine(128),128);auto bytes=wave.waveFile();auto restored=dsp::Wavetable::import(bytes);check(restored.frames()==1,"Single cycle became a table bank");for(unsigned n=0;n<2048;++n){const double phase=n/2048.;check(std::abs(wave.read(0,phase,110,48000)-restored.read(0,phase,110,48000))<1.e-5,"WAV export altered a prepared cycle");}for(const auto size:{std::size_t(0),std::size_t(8),bytes.size()-1})rejects([&]{dsp::Wavetable::import(std::span(bytes).first(size));});
 rejects([&]{dsp::Wavetable::fromCycles(sine(4096,1300),4096);});rejects([&]{dsp::Wavetable::fromCycles(sine(15),15);}); // Downsampling must not fold an out-of-band harmonic into an audible one.

}
void portableWT(){ByteWriter bytes;bytes.u32(0x74776176);bytes.u32(32);bytes.bytes.push_back(std::byte{2});bytes.bytes.push_back(std::byte{});bytes.bytes.push_back(std::byte{12});bytes.bytes.push_back(std::byte{});for(unsigned frame=0;frame<2;++frame)for(unsigned n=0;n<32;++n){const auto value=std::int16_t(std::lround(std::sin(2*std::numbers::pi*(frame+1)*n/32)*30000));bytes.bytes.push_back(std::byte(unsigned(value)&255));bytes.bytes.push_back(std::byte((unsigned(value)>>8)&255));}const auto wave=dsp::Wavetable::import(bytes.bytes);check(wave.frames()==2,"WT dimensions or int16 flag was ignored");bytes.bytes[10]=std::byte{1};rejects([&]{dsp::Wavetable::import(bytes.bytes);});}
void playback(){auto wave=dsp::Wavetable::fromCycles(sine(2048),2048);double energy=0;cell_test::allocations=cell_test::deletions=0;cell_test::audio=true;for(unsigned n=0;n<48000;++n){const auto value=wave.read(.4f,n*440./48000,440,48000);if(!std::isfinite(value)){cell_test::audio=false;throw std::runtime_error("Nonfinite wavetable playback");}energy+=value*value;}cell_test::audio=false;check(energy>1000&&!cell_test::allocations&&!cell_test::deletions,"Wavetable playback was silent or used callback storage");}
void glide(){dsp::Portamento glide;glide.reset(48);glide.target(72,.1,1000,0,true);double previous=48;for(unsigned n=0;n<100;++n){const auto value=glide.next();check(value>=previous&&value<=72,"Glide jumped or overshot");previous=value;}check(std::abs(previous-72)<1.e-9,"Glide time did not reach its destination");glide.target(60,.2,1000,.8,false);check(glide.next()==60,"Disabled glide did not move immediately");}
void fm(){dsp::FmGraph graph;dsp::FmVoice voice;for(unsigned mode=0;mode<8;++mode){dsp::fmAlgorithm(graph,mode);for(auto& op:graph.operators){op.attack=.001f;op.feedback=3;}voice.reset();voice.noteOn();double energy=0;for(unsigned n=0;n<4096;++n){const auto value=voice.next(880,48000,graph,2);check(std::isfinite(value),"FM feedback produced nonfinite output");energy+=value*value;}check(energy>1.e-6,"An FM topology was silent");voice.noteOff(graph,48000);}}
}
int main(){try{roundtrip();portableWT();playback();glide();fm();std::cout<<"PASS wavetable interchange, callback ownership, portamento and bounded FM graphs\n";}catch(const std::exception& e){cell_test::audio=false;std::cerr<<e.what()<<'\n';return 1;}}
