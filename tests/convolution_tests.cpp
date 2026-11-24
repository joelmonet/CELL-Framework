#include <cell/convolution.hpp>
#include <bit>
#include <fstream>
#include <iostream>
#include <random>

namespace {
void require(bool value,const char* message){if(!value)throw std::runtime_error(message);}
double verify(std::span<const float> kernel,std::uint32_t partition) {
    cell::dsp::Convolver convolution;convolution.prepare(kernel,partition);
    require(convolution.latency()==0,"convolution introduces latency");
    std::mt19937 random(192837);std::uniform_real_distribution<float> distribution(-.1f,.1f);
    std::vector<float> input(kernel.size()*2+513),output(input.size());
    for(auto& value:input)value=distribution(random);
    std::size_t position=0;
    for(std::size_t chunk:{1u,17u,193u,65u,511u}) {
        const auto count=std::min(chunk,input.size()-position);
        for(std::size_t i=0;i<count;++i)output[position+i]=convolution.process(input[position+i]);
        position+=count;if(position==input.size())break;
    }
    while(position<input.size()){output[position]=convolution.process(input[position]);++position;}
    double maximum=0;
    for(std::size_t n=0;n<input.size();++n){double expected=0;
        for(std::size_t tap=0;tap<kernel.size()&&tap<=n;++tap)expected+=double(kernel[tap])*input[n-tap];
        maximum=std::max(maximum,std::abs(expected-output[n]));}
    require(maximum<3e-5,"partitioned convolution differs from direct reference");
    convolution.reset();for(std::size_t i=0;i<kernel.size()+partition;++i)require(convolution.process(0)==0,"reset leaves convolution tail");
    convolution.reset();
    for(std::size_t i=0;i<kernel.size();++i)require(std::abs(convolution.process(i==0?1.f:0.f)-kernel[i])<3e-5,"impulse response changed or shifted");
    return maximum;
}
std::uint32_t u32(std::istream& stream){unsigned char b[4]{};stream.read(reinterpret_cast<char*>(b),4);return std::uint32_t(b[0])|(std::uint32_t(b[1])<<8)|(std::uint32_t(b[2])<<16)|(std::uint32_t(b[3])<<24);}
}
int main(int argc,char** argv){
    try{
        std::mt19937 random(12345);std::uniform_real_distribution<float> distribution(-.2f,.2f);
        double maximum=0;
        for(std::size_t length:{1u,63u,64u,65u,128u,257u,1024u}){
            std::vector<float> kernel(length);for(auto& v:kernel)v=distribution(random);
            for(std::uint32_t partition:{32u,64u,128u})maximum=std::max(maximum,verify(kernel,partition));
        }
        // Deferred zero-padding regression: preserve the full audible FIR
        // reference and declared tail, while reporting less processing work.
        for(std::size_t padding:{1u,64u,1024u})for(std::uint32_t partition:{32u,64u,128u}){
            std::vector<float> padded(137+padding);padded[0]=.5f;padded[7]=-.25f;padded[136]=.125f;cell::dsp::Convolver compact;compact.prepare(padded,partition);
            require(compact.processingSamples()==137&&compact.tailSamples()==padded.size()-1&&compact.latency()==0,"Exact-zero compaction changed reported tail/latency or retained padding work");maximum=std::max(maximum,verify(padded,partition));
        }
        for(float value:{0.f,1.f}){std::vector<float> padded(2048);padded[0]=value;cell::dsp::Convolver compact;compact.prepare(padded);require(compact.processingSamples()==1&&compact.tailSamples()==2047,"Head-only IR retained a zero FFT tail or changed declared duration");for(unsigned n=0;n<4096;++n)require(compact.process(n==0?1.f:0.f)==(n==0?value:0.f),"Head-only compact IR changed an impulse or its silence");}
        std::uint32_t references=0;
        if(argc==2){
            std::ifstream file(argv[1],std::ios::binary);char magic[4]{};file.read(magic,4);
            require(std::string_view(magic,4)=="CIR1","invalid IR fixture");references=u32(file);
            require(references>0&&references<=256,"invalid reference count");
            for(std::uint32_t i=0;i<references;++i){const auto length=u32(file);require(length>0&&length<65536,"invalid reference length");
                std::vector<float> kernel(length);for(auto& value:kernel)value=std::bit_cast<float>(u32(file));require(bool(file),"truncated reference fixture");
                maximum=std::max(maximum,verify(kernel,64));}
        }
        std::cout<<"PASS: zero-latency convolution, direct FIR comparison, variable block boundaries, exact tail reset; "<<references<<" supplied HRIR channels; max error "<<maximum<<".\n";
    }catch(const std::exception& e){std::cerr<<e.what()<<'\n';return 1;}
}
