// Original Cell Framework implementation; see LICENSE.
#include <cell/audio_file.hpp>
#include <cell/uuid.hpp>
#include <atomic>
#include <iomanip>
#include <iostream>
#include <random>
#include <thread>
namespace {void need(bool value,const char* message){if(!value)throw std::runtime_error(message);}}
int main(){cell::File root;try{
    using namespace cell::audio;
    if(!compressedAudioCodecsAvailable()){std::cout<<"Native optional codec provider unavailable\n";return 77;}
    root=cell::File::getSpecialLocation(cell::File::tempDirectory).getChildFile("cell-audio-codec-"+cell::Uuid().toString());need(root.createDirectory().wasOk(),"Cannot create codec fixtures");
    for(const unsigned bits:{16u,24u}){
        constexpr int frames=8193;std::array<std::vector<float>,2> source{std::vector<float>(frames),std::vector<float>(frames)};std::mt19937 random(bits);
        for(auto& channel:source)for(auto& value:channel){const int bound=1<<(bits-2);value=float(int(random()%unsigned(bound*2))-bound)/float(1<<(bits-1));}
        const float* input[]{source[0].data(),source[1].data()};const auto file=root.getChildFile("lossless-"+cell::Text(bits)+".flac");auto writer=createFlacFile(file,48000,2,bits,5);need(writer->write(input,2,frames)&&writer->framesWritten()==frames&&writer->finish().wasOk(),"Native FLAC encode/finalization failed");writer.reset();const auto reader=openAudioFile(file);need(reader->info().channels==2&&reader->info().frames==frames&&reader->info().sampleRate==48000,"FLAC metadata changed");
        std::array<std::vector<float>,2> decoded{std::vector<float>(frames),std::vector<float>(frames)};float* output[]{decoded[0].data(),decoded[1].data()};const bool read=reader->read(output,2,0,frames);if(!read)std::cerr<<"Decode status: "<<reader->status().getErrorMessage()<<'\n';if(read&&decoded!=source){for(std::size_t c=0;c<2;++c)for(int n=0;n<frames;++n)if(decoded[c][std::size_t(n)]!=source[c][std::size_t(n)]){std::cerr<<"FLAC difference bits="<<bits<<" channel="<<c<<" frame="<<n<<" source="<<std::setprecision(12)<<source[c][std::size_t(n)]<<" got="<<decoded[c][std::size_t(n)]<<'\n';goto differenceReported;}differenceReported:;}
        need(read&&decoded==source,"Native FLAC changed exact integer-resolution samples");
        std::atomic<bool> okay{true};std::vector<std::thread> workers;
        for(int n=0;n<6;++n)workers.emplace_back([&,n]{std::array<float,257> values{};float* channels[]{n%2==0?values.data():nullptr,n%2?values.data():nullptr};const auto first=n*1000;if(!reader->read(channels,2,first,int(values.size()))||!std::equal(values.begin(),values.end(),source[std::size_t(n%2)].begin()+first))okay=false;});
        for(auto& thread:workers)thread.join();need(okay,"Concurrent native codec reads raced the shared cursor");
        struct Cancelled:std::runtime_error{Cancelled():std::runtime_error("cancel"){} };
        bool cancelled{};unsigned checks{};
        try{reader->read(output,2,4000,257,[&]{if(++checks==2)throw Cancelled();});}catch(const Cancelled&){cancelled=true;}
        need(cancelled,"Native codec lost the cancellation exception across its C I/O callbacks");
        need(reader->read(output,2,0,frames)&&decoded==source,"A cancelled native read poisoned later playback");
    }
    {
        const std::array<float,3> samples{0.f,.25f,-.25f};const float* input[]{samples.data()};
        auto writer=createFlacFile(root.getChildFile("cancelled.flac"),48000,1,24);
        struct Stop{};bool stopped{};
        try{writer->write(input,1,3,[]{throw Stop{};});}catch(const Stop&){stopped=true;}
        need(stopped&&writer->status().failed()&&writer->finish().failed()&&!writer->write(input,1,3),"Cancelled FLAC output was accepted as complete");
    }
    need(root.deleteRecursively(),"Cannot remove owned codec fixtures");std::cout<<"PASS native codec provider: "<<compressedAudioCodecVersion()<<",FLAC16/24 exact samples,final headers,metadata,concurrent seeking,cancellation recovery and cancelled output rejection\n";return 0;
}catch(const std::exception& error){root.deleteRecursively();std::cerr<<error.what()<<'\n';return 1;}}
