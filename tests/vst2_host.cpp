#include <cell/audio.hpp>
#include "pluginterfaces/vst2.x/aeffectx.h"
#include <cstring>
#include <iostream>
#include <vector>
#if defined(_WIN32)
#include <windows.h>
#else
#include <dlfcn.h>
#endif

namespace {
void check(bool condition,const char* message){if(!condition)throw std::runtime_error(message);}
VstIntPtr hostVersion=2400;
VstTimeInfo timeInfo{};
VstIntPtr VSTCALLBACK host(AEffect*,VstInt32 opcode,VstInt32,VstIntPtr,void*,float){
    if(opcode==audioMasterVersion)return hostVersion;
    if(opcode==audioMasterGetTime)return reinterpret_cast<VstIntPtr>(&timeInfo);
    if(opcode==audioMasterGetSampleRate)return 48000;
    if(opcode==audioMasterGetBlockSize)return 7;
    return 0;
}
struct Module {
    using Factory=AEffect* (VSTCALLBACK *)(audioMasterCallback);
#if defined(_WIN32)
    HMODULE handle{};
#else
    void* handle{};
#endif
    Factory factory{};
    explicit Module(const char* path){
#if defined(_WIN32)
        handle=LoadLibraryA(path);if(handle)factory=reinterpret_cast<Factory>(GetProcAddress(handle,"VSTPluginMain"));
#else
        handle=dlopen(path,RTLD_NOW|RTLD_LOCAL);if(handle)factory=reinterpret_cast<Factory>(dlsym(handle,"VSTPluginMain"));
#endif
    }
    ~Module(){
#if defined(_WIN32)
        if(handle)FreeLibrary(handle);
#else
        if(handle)dlclose(handle);
#endif
    }
};
struct Instance {
    AEffect* effect{};
    ~Instance(){if(effect){call(effMainsChanged,0,0);call(effClose);}}
    VstIntPtr call(VstInt32 opcode,VstInt32 index=0,VstIntPtr value=0,void* pointer=nullptr,float option=0){
        return effect->dispatcher(effect,opcode,index,value,pointer,option);
    }
};
void exercise(const char* path){
    Module module(path);check(module.handle&&module.factory,"could not load VST2 factory");
    check(!module.factory(nullptr),"factory accepted a missing host");
    hostVersion=2300;check(!module.factory(host),"factory accepted a pre-2.4 host");hostVersion=2400;
    Instance instance{module.factory(host)};auto* effect=instance.effect;
    check(effect&&effect->magic==kEffectMagic&&effect->uniqueID==VstInt32(0x43474578),"VST2 ABI/identity mismatch");
    check(effect->numParams==1&&effect->numInputs==2&&effect->numOutputs==2&&effect->processReplacing,"gain profile ports");
    check(instance.call(effOpen)==1&&instance.call(effGetVstVersion)==2400,"VST2 open/version");
    char name[kVstMaxEffectNameLen]{};check(instance.call(effGetEffectName,0,0,name)==1&&std::strstr(name,"Cell Gain"),"VST2 name");
    check(instance.call(effSetSampleRate,0,0,nullptr,48000)==1&&instance.call(effSetBlockSize,0,7)==1,"VST2 setup");
    effect->setParameter(effect,0,.75f); // -6 dB in the example's -60..+12 range
    check(std::abs(effect->getParameter(effect,0)-.75f)<1e-6,"VST2 normalized round trip");
    effect->setParameter(effect,1,0);check(effect->getParameter(effect,1)==0,"unknown parameter access");
    check(instance.call(effMainsChanged,0,1)==1&&instance.call(effStartProcess)==1,"VST2 mains/start");
    timeInfo.sampleRate=48000;timeInfo.tempo=93;timeInfo.ppqPos=9;timeInfo.samplePos=100;
    timeInfo.timeSigNumerator=7;timeInfo.timeSigDenominator=8;timeInfo.flags=kVstTempoValid|kVstPpqPosValid|kVstTimeSigValid|kVstTransportPlaying|kVstTransportCycleActive;
    std::vector<float> left(1025,1),right(1025,2);float* buffers[]{left.data(),right.data()};
    effect->processReplacing(effect,buffers,buffers,VstInt32(left.size()));
    const auto gain=cell::dbToGain(-6);
    for(std::size_t frame=0;frame<left.size();++frame)check(std::abs(left[frame]-gain)<2e-6f&&std::abs(right[frame]-2*gain)<3e-6f,"oversized in-place VST2 block cutoff or gain error");
    void* borrowed{};const auto length=instance.call(effGetChunk,0,0,&borrowed);
    check(length>0&&borrowed,"VST2 active state save");
    std::vector<std::byte> state(static_cast<const std::byte*>(borrowed),static_cast<const std::byte*>(borrowed)+length);
    check(instance.call(effSetChunk,0,length,state.data())==0,"VST2 generic restore bypassed stopped contract");
    check(instance.call(effStopProcess)==1,"VST2 stop");
    left.assign(left.size(),1);right.assign(right.size(),2);buffers[0]=left.data();buffers[1]=right.data();
    effect->processReplacing(effect,buffers,buffers,VstInt32(left.size()));
    check(left.front()==0&&left.back()==0&&right.back()==0,"stopped VST2 render left stale output");
    check(instance.call(effMainsChanged,0,0)==1,"VST2 suspend");
    effect->setParameter(effect,0,.5f);
    check(instance.call(effSetChunk,0,length,state.data())==1&&std::abs(effect->getParameter(effect,0)-.75f)<1e-6,"VST2 state recall");
    auto corrupt=state;corrupt[0]^=std::byte(1);
    check(instance.call(effSetChunk,0,VstIntPtr(corrupt.size()),corrupt.data())==0&&std::abs(effect->getParameter(effect,0)-.75f)<1e-6,"malformed VST2 state changed parameters");
    VstSpeakerArrangement mono{};mono.type=kSpeakerArrMono;mono.numChannels=1;
    check(instance.call(effSetSpeakerArrangement,0,reinterpret_cast<VstIntPtr>(&mono),&mono)==1&&effect->numInputs==1&&effect->numOutputs==1,"VST2 mono negotiation");
    check(instance.call(effSetSampleRate,0,0,nullptr,96000)==1&&instance.call(effMainsChanged,0,1)==1,"VST2 reprepare");
    left.assign(23,1);float* monoBuffer[]{left.data()};effect->processReplacing(effect,monoBuffer,monoBuffer,23);
    for(float value:left)check(std::abs(value-gain)<2e-6f,"VST2 mono recall/reprepare");
    check(instance.call(effEditOpen,0,0,reinterpret_cast<void*>(1))==0,"headless VST2 advertised an editor");
}
}
int main(int argc,char** argv){
    try{check(argc==2,"usage: vst2_host gain-plugin-file");exercise(argv[1]);std::cout<<"PASS: VST2 factory, ports, parameters, oversized blocks, state, mono and lifecycle\n";}
    catch(const std::exception& error){std::cerr<<error.what()<<'\n';return 1;}
}
