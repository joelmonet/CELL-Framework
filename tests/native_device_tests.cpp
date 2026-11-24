#include <cell/audio_device.hpp>
#include <atomic>
#include <chrono>
#include <cmath>
#include <fstream>
#include <iostream>
#include <thread>
#include <vector>
struct Probe{
 std::uint64_t frames{};bool unexpectedInput{};std::vector<float>samples=std::vector<float>(48000*8*2);bool overflow{};
 static void render(void*owner,float*out,const float*in,unsigned n)noexcept{
  auto&p=*static_cast<Probe*>(owner);p.unexpectedInput=p.unexpectedInput||in;
  for(unsigned f=0;f<n;++f){const float sample=float(.2*std::sin(double(p.frames+f)*6.283185307179586*997/48000));out[f*2]=sample;out[f*2+1]=-sample;
   if((p.frames+f)*2+1<p.samples.size()){p.samples[(p.frames+f)*2]=sample;p.samples[(p.frames+f)*2+1]=-sample;}else p.overflow=true;}
  p.frames+=n;
 }
};
int main(int argc,char**argv){try{
 Probe p;cell::audio::Device device;cell::audio::DeviceConfig config;config.backend=cell::audio::Backend::pulse;config.sampleRate=48000;config.inputChannels=0;config.periodFrames=128;config.outputChannels=2;
 auto unsupported=config;unsupported.outputChannels=64;bool rejected=false;try{device.open(unsupported,Probe::render,&p);}catch(...){rejected=true;}if(!rejected||device.isOpen())throw std::runtime_error("Failed channel negotiation did not close its partial device");
 const auto format=device.open(config,Probe::render,&p);if(format.sampleRate!=48000||format.inputChannels||format.outputChannels!=2||format.captureBridgeCapacityFrames)throw std::runtime_error("Playback-only endpoint allocated capture state or changed the sample clock");
 for(int cycle=0;cycle<3;++cycle){if(!device.start())throw std::runtime_error(device.error());std::this_thread::sleep_for(std::chrono::milliseconds(400));device.stop();const auto stopped=p.frames;std::this_thread::sleep_for(std::chrono::milliseconds(10));if(p.frames!=stopped||device.stats().running)throw std::runtime_error("Device stop returned before its audio callback joined");}
 const auto stats=device.stats();device.close();std::cout<<"device diagnostics: frames="<<p.frames<<" overflow="<<p.overflow<<" unexpectedInput="<<p.unexpectedInput<<" missing="<<stats.missingInputCallbacks<<" callbacks="<<stats.callbacks<<" overruns="<<stats.callbackOverruns<<" interrupted="<<stats.interrupted<<"\n";if(p.frames<48000/2||p.overflow||p.unexpectedInput||stats.missingInputCallbacks)throw std::runtime_error("Playback-only callback stalled, exceeded storage, or depended on capture");
 if(argc>1){std::ofstream out(argv[1],std::ios::binary);out.write(reinterpret_cast<const char*>(p.samples.data()),std::streamsize(p.frames*2*sizeof(float)));if(!out)throw std::runtime_error("Trace write failed");}
 std::cout<<"PASS native PulseAudio playback-only / 128 requested frames / 48 kHz, three start-stop cycles, joined callbacks, zero capture bridge and missing-input counters; callbacks="<<stats.callbacks<<" frames="<<p.frames<<" output-buffer estimate="<<format.outputBufferEstimate()<<". Clocked virtual endpoint; no physical-speaker latency claim.\n";
 return 0;
}catch(const std::exception&e){std::cerr<<"FAIL native device: "<<e.what()<<'\n';return 1;}}
