// SPDX-License-Identifier: Apache-2.0
// Independent installed-SDK consumer. Device transfer fixture, no product DSP.
#include <cell/audio_device.hpp>
#include <cell/files.hpp>
#include <algorithm>
#include <chrono>
#include <iostream>
#include <thread>
struct Probe {
    std::vector<float> capture;std::size_t frames{};bool overflow{};
    Probe():capture(48000*10*2){}
    static void callback(void* owner,float* output,const float* input,unsigned count)noexcept{
        auto& p=*static_cast<Probe*>(owner);
        for(unsigned n=0;n<count*2;++n)output[n]=input?input[n]:0;
        if(count>p.capture.size()/2-p.frames){p.overflow=true;return;}
        std::copy_n(output,count*2,p.capture.data()+p.frames*2);p.frames+=count;
    }
};
int main(int argc,char** argv){try{
    if(argc!=3)throw std::invalid_argument("Arguments: playback-endpoint capture-output.f32");
    Probe probe;cell::audio::Device device;cell::audio::DeviceConfig config;config.backend=cell::audio::Backend::pulse;config.input="cell_reaper_capture.monitor";config.output=argv[1];
    device.open(config,Probe::callback,&probe);if(!device.start())throw std::runtime_error(device.error());
    const auto end=std::chrono::steady_clock::now()+std::chrono::seconds(8);while(std::chrono::steady_clock::now()<end)std::this_thread::sleep_for(std::chrono::milliseconds(10));
    device.stop();if(probe.overflow)throw std::runtime_error("Probe exceeded the prepared capture capacity");
    cell::files::atomicWrite(argv[2],std::as_bytes(std::span(probe.capture.data(),probe.frames*2)),true);
    std::cout<<"Native callbacks="<<device.stats().callbacks<<" frames="<<probe.frames<<"\n";
}catch(const std::exception& error){std::cerr<<error.what()<<'\n';return 1;}}
