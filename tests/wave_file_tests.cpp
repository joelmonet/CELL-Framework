#include "sparse_fixture.hpp"
// Original Cell Framework implementation; see LICENSE.
#include <cell/audio_file.hpp>
#include <cell/uuid.hpp>
#include "../src/audio_file_internal.hpp"
#include <atomic>
#include <bit>
#include <cmath>
#include <iostream>
#include <random>
#include <thread>
namespace {
void need(bool value,const char* message){if(!value)throw std::runtime_error(message);}
template<class Action>void rejects(Action action){bool failed{};try{action();}catch(const std::runtime_error&){failed=true;}need(failed,"Invalid WAV was accepted");}
void number(std::vector<std::uint8_t>& data,std::uint64_t value,unsigned bytes,bool big=false){const auto at=data.size();data.resize(at+bytes);for(unsigned n=0;n<bytes;++n)data[at+(big?bytes-1-n:n)]=std::uint8_t(value>>(n*8));}
void text(std::vector<std::uint8_t>& data,const char* value){data.insert(data.end(),value,value+4);}
std::vector<std::uint8_t> golden(bool big,unsigned encoding,unsigned bits,std::span<const std::uint8_t> samples){
    std::vector<std::uint8_t> result;text(result,big?"RIFX":"RIFF");number(result,36+samples.size()+(samples.size()&1),4,big);text(result,"WAVE");text(result,"fmt ");number(result,16,4,big);number(result,encoding,2,big);number(result,1,2,big);number(result,48000,4,big);number(result,48000*((bits+7)/8),4,big);number(result,(bits+7)/8,2,big);number(result,bits,2,big);text(result,"data");number(result,samples.size(),4,big);result.insert(result.end(),samples.begin(),samples.end());if(samples.size()&1)result.push_back(0);return result;
}
}
int main(){cell::File root;try{
    using namespace cell::audio;
    root=cell::File::getSpecialLocation(cell::File::tempDirectory).getChildFile("cell-wave-"+cell::Uuid().toString());need(root.createDirectory().wasOk(),"Cannot create owned WAV fixtures");
    for(const bool big:{false,true}){
        const std::array<std::uint8_t,6> little{0,0x80,0,0,0xff,0x7f},be{0x80,0,0,0,0x7f,0xff};const auto data=golden(big,1,16,big?std::span(be):std::span(little));const auto file=root.getChildFile(big?"golden-rifx.wav":"golden-riff.wav");need(file.replaceWithData(data.data(),data.size()),"Cannot write golden input");const auto reader=openAudioFile(file);std::array<double,3> samples{},extra{9,9,9};double* outputs[]{samples.data(),extra.data()};need(reader->info().frames==3&&reader->read(outputs,2,0,3)&&samples[0]==-1.&&samples[1]==0.&&samples[2]==32767./32768.&&extra==std::array<double,3>{},"Known PCM/endian or extra-channel decode changed");
    }
    {
        std::vector<std::uint8_t> raw;for(const double value:{-0.,1.25,-2.5,.123456789012345}){const auto bits=std::bit_cast<std::uint64_t>(value);number(raw,bits,8,true);}const auto bytes=golden(true,3,64,raw);const auto file=root.getChildFile("golden-float64-rifx.wav");need(file.replaceWithData(bytes.data(),bytes.size()),"Cannot write float64 fixture");const auto reader=openAudioFile(file);std::array<double,4> got{};double* outputs[]{got.data()};need(reader->read(outputs,1,0,4)&&std::signbit(got[0])&&got[1]==1.25&&got[2]==-2.5&&got[3]==.123456789012345,"Float64 precision,headroom or signed zero changed");
    }
    for(const unsigned channels:{1u,2u,32u,64u,1921u}){
        const int frames=257;WaveFileOptions options;options.channels=channels;options.bitsPerSample=channels==1921?24:32;options.floatingPoint=channels!=1921;options.timeReferenceSamples=UINT64_C(123456789012345);const auto file=root.getChildFile("roundtrip-"+cell::Text(channels)+".wav");
        std::vector<std::vector<float>> data(channels,std::vector<float>(frames));std::vector<const float*> pointers(channels);
        for(unsigned c=0;c<channels;++c){for(int n=0;n<frames;++n)data[c][std::size_t(n)]=float((int(c%31)-15)/32.+(n%5)/32.);pointers[c]=data[c].data();}
        if(options.floatingPoint)data[0][0]=1.25f;
        auto writer=createWaveFile(file,options);need(writer->write(pointers.data(),channels,frames)&&writer->framesWritten()==frames&&writer->finish().wasOk(),"Native WAV write/finalization failed");writer.reset();const auto reader=openAudioFile(file);need(reader->info().channels==channels&&reader->info().frames==frames&&reader->info().timeReferenceSamples==options.timeReferenceSamples,"WAV channels,length or BWAV origin changed");
        std::vector<float*> targets(channels,nullptr);std::array<float,frames> first{},last{};targets[0]=first.data();if(channels>1)targets.back()=last.data();need(reader->read(targets.data(),channels,0,frames),"Selective native WAV read failed");need(std::equal(first.begin(),first.end(),data.front().begin())&&(channels==1||std::equal(last.begin(),last.end(),data.back().begin())),"Native WAV altered samples or skipped the final channel");
        need(reader->workingMemoryBytes()<2*1024*1024,"Reader working memory grew with recording length");
    }
    for(const unsigned bits:{8u,16u,24u,32u,64u}){
        WaveFileOptions options;options.channels=1;options.bitsPerSample=bits;options.floatingPoint=bits>=32;const auto file=root.getChildFile("precision-"+cell::Text(bits)+".wav");const std::array<double,5> data{-1.,-.5,0.,.5,bits>=32?1.25:.75};const double* ptr[]{data.data()};auto writer=createWaveFile(file,options);need(writer->write(ptr,1,5)&&writer->finish().wasOk(),"WAV precision output failed");writer.reset();auto reader=openAudioFile(file);std::array<double,5> decoded{};double* destination[]{decoded.data()};need(reader->read(destination,1,0,5)&&decoded==data,"Known exact PCM/float samples did not survive output");
    }
    {
        WaveFileOptions options;options.channels=32;options.sampleRate=192000;options.timeReferenceSamples=UINT64_C(1)<<40;
        constexpr std::int64_t frames=INT64_C(192000)*4*3600;const auto header=waveFileHeader(options,frames);need(std::memcmp(header.getData(),"RF64",4)==0,"Long native header did not promote to RF64");const auto file=root.getChildFile("long-32-channel-sparse.wav");
        constexpr int count=257;const std::array<std::int64_t,4> positions{0,frames/2,(INT64_C(1)<<31)-64,frames-count};
        {auto output=file.createOutputStream(cellTest::prepareSparseFixture(file));need(bool(output)&&output->write(header.getData(),header.getSize()),"Cannot create sparse RF64 header");const auto bytes=std::uint64_t(frames)*32*4;need(output->setPosition(std::int64_t(header.getSize()+bytes-1))&&output->writeByte(0),"64-bit sparse extension failed");for(const auto first:positions){need(output->setPosition(std::int64_t(header.getSize())+first*32*4),"64-bit marker seek failed");for(int n=0;n<count;++n)for(int c=0;c<32;++c)need(output->writeFloat(c/64.f+((n==0||n==127||n==count-1)?.25f:0.f)),"Sparse marker write failed");}output->flush();need(output->getStatus().wasOk(),"Sparse marker flush failed");}
        const auto reader=openAudioFile(file);need(reader->info().channels==32&&reader->info().frames==frames&&reader->info().sampleRate==192000&&reader->info().timeReferenceSamples==options.timeReferenceSamples,"Long RF64 metadata narrowed a count");std::atomic<bool> okay{true};std::vector<std::thread> workers;
        for(int channel:{0,15,31})workers.emplace_back([&,channel]{std::array<float,count> values{};std::array<float*,32> outputs{};outputs[std::size_t(channel)]=values.data();for(const auto first:positions){if(!reader->read(outputs.data(),32,first,count)){okay=false;return;}for(int n=0;n<count;++n)if(values[std::size_t(n)]!=channel/64.f+((n==0||n==127||n==count-1)?.25f:0.f))okay=false;}});
        for(auto& worker:workers)worker.join();need(okay,"Concurrent/distant RF64 reads raced or changed samples");need(reader->workingMemoryBytes()<1024*1024,"Sparse RF64 allocated whole-recording storage");
    }
    {
        const std::array<std::uint8_t,6> raw{0,0,0,0,0,0};const auto seed=golden(false,1,16,raw);const auto file=root.getChildFile("malformed.wav");
        for(std::size_t cut=0;cut<seed.size();++cut){need(file.replaceWithData(seed.data(),cut),"Cannot write truncated fixture");rejects([&]{(void)cell::audio::detail::openWaveFile(file,{});});}
        std::mt19937 random(0x574156);for(unsigned iteration=0;iteration<500;++iteration){auto mutated=seed;for(unsigned n=0;n<1+random()%3;++n)mutated[12+random()%(mutated.size()-12)]^=std::uint8_t(1u<<(random()%8));need(file.replaceWithData(mutated.data(),mutated.size()),"Cannot write mutated fixture");try{const auto reader=cell::audio::detail::openWaveFile(file,{});if(reader->info().frames){std::vector<float> sample(reader->info().channels);std::vector<float*> out(sample.size());for(std::size_t c=0;c<out.size();++c)out[c]=&sample[c];(void)reader->read(out.data(),out.size(),0,1);}}catch(const std::runtime_error&){} }
    }
    {
        WaveFileOptions invalid;invalid.channels=0;const auto absent=root.getChildFile("invalid-output.wav");rejects([&]{(void)createWaveFile(absent,invalid);});need(!absent.exists(),"Invalid format left an output file");
        const auto existing=root.getChildFile("preserve.wav");need(existing.replaceWithText("preserve source"),"Cannot prepare collision");rejects([&]{(void)createWaveFile(existing,WaveFileOptions{});});need(existing.loadFileAsString()=="preserve source","Exclusive writer replaced existing media");
    }
    {
        const std::array<float,3> samples{0.f,.25f,-.25f};const float* input[]{samples.data()};
        WaveFileOptions options;options.channels=1;auto writer=createWaveFile(root.getChildFile("cancelled.wav"),options);
        struct Stop{};bool stopped{};
        try{writer->write(input,1,3,[]{throw Stop{};});}catch(const Stop&){stopped=true;}
        need(stopped&&writer->status().failed()&&writer->finish().failed()&&!writer->write(input,1,3),"Cancelled WAV output was accepted as complete");
    }
    need(root.deleteRecursively(),"Cannot remove owned WAV fixtures");std::cout<<"PASS native WAV: PCM8/16/24,float32/64,RIFX,exact headroom,BWAV origins,32/64/1921channels,shared concurrent four-hour192kHz RF64 seeks beyond2^31,malformed input,exclusive output and cancelled output rejection\n";return 0;
}catch(const std::exception& error){root.deleteRecursively();std::cerr<<error.what()<<'\n';return 1;}}
