// Original Cell Framework implementation; see LICENSE.
#include "audio_file_internal.hpp"
#include <algorithm>
#include <bit>
#include <cmath>
#include <cstring>
#include <mutex>

namespace cell::audio {
namespace {
using detail::require;
constexpr std::size_t transferBytes=262144;
std::uint64_t integer(const std::uint8_t* bytes,unsigned count,bool big=false) noexcept {
    std::uint64_t value{};for(unsigned n=0;n<count;++n)value|=std::uint64_t(bytes[n])<<(8*(big?count-1-n:n));return value;
}
void put(std::uint8_t* bytes,std::uint64_t value,unsigned count) noexcept {for(unsigned n=0;n<count;++n){bytes[n]=std::uint8_t(value&255);value>>=8;}}
std::uint32_t tag(const char* name) noexcept {return std::uint32_t(integer(reinterpret_cast<const std::uint8_t*>(name),4));}
class WaveReader final:public AudioFileReader {
    struct Segment {std::int64_t first{},frames{};std::uint64_t offset{};};
    std::unique_ptr<InputStream> input_;AudioFileInfo info_;unsigned sampleBytes_{},frameBytes_{};bool big_{};
    std::vector<Segment> segments_;std::vector<std::uint8_t> buffer_;
    std::vector<std::size_t> active_;mutable std::mutex mutex_;Result status_=Result::ok();
    void readAt(std::uint64_t offset,void* data,std::size_t size) {
        require(offset<=std::uint64_t(INT64_MAX)&&size<=std::uint64_t(input_->getTotalLength())-std::min(offset,std::uint64_t(input_->getTotalLength())),"WAV field exceeds its file");
        require(input_->setPosition(std::int64_t(offset))&&input_->readFully(std::span(static_cast<std::byte*>(data),size)),"Cannot read WAV data");
    }
    double sample(const std::uint8_t* data)const noexcept {
        const auto bits=integer(data,sampleBytes_,big_);
        if(info_.floatingPoint)return sampleBytes_==4?double(std::bit_cast<float>(std::uint32_t(bits))):std::bit_cast<double>(bits);
        if(sampleBytes_==1)return (double(bits)-128.)/128.;
        const auto width=sampleBytes_*8;const auto sign=UINT64_C(1)<<(width-1);
        const auto value=(bits&sign)?std::int64_t(bits)-std::int64_t(UINT64_C(1)<<width):std::int64_t(bits);
        return double(value)*std::ldexp(1.,1-int(width));
    }
    template<class Value>bool readImpl(Value*const* outputs,std::size_t channels,std::int64_t first,int count,const FileWorkCheck& check) {
        const std::lock_guard lock(mutex_);status_=Result::ok();
        if((channels&&!outputs)||first<0||count<0||first>info_.frames||std::int64_t(count)>info_.frames-first){status_=Result::fail("Invalid WAV read range");return false;}
        active_.clear();
        for(std::size_t c=0;c<std::min<std::size_t>(channels,info_.channels);++c)if(outputs[c])active_.push_back(c);
        for(std::size_t c=info_.channels;c<channels;++c)if(outputs[c])std::fill_n(outputs[c],count,Value{});
        if(!count||active_.empty())return true;
        const auto maximum=std::max<std::size_t>(1,transferBytes/frameBytes_);
        int done{};
        auto segment=std::upper_bound(segments_.begin(),segments_.end(),first,[](auto value,const Segment& part){return value<part.first;});
        if(segment!=segments_.begin())--segment;
        while(done<count) {
            detail::work(check);
            const auto position=first+done;
            while(segment!=segments_.end()&&position>=segment->first+segment->frames)++segment;
            if(segment==segments_.end()){status_=Result::fail("Missing WAV audio segment");return false;}
            const auto amount=int(std::min<std::uint64_t>({std::uint64_t(count-done),std::uint64_t(segment->first+segment->frames-position),maximum}));
            buffer_.resize(std::size_t(amount)*frameBytes_);
            const auto offset=segment->offset+std::uint64_t(position-segment->first)*frameBytes_;
            if(!input_->setPosition(std::int64_t(offset))||!input_->readFully(std::span(reinterpret_cast<std::byte*>(buffer_.data()),buffer_.size()))) {status_=Result::fail("WAV source read failed");return false;}
            for(const auto c:active_)for(int n=0;n<amount;++n) {
                const auto value=sample(buffer_.data()+std::size_t(n)*frameBytes_+c*sampleBytes_);
                const auto converted=Value(value);
                if(!std::isfinite(value)||!std::isfinite(converted)){status_=Result::fail("Non-finite or unrepresentable WAV sample");return false;}
                outputs[c][done+n]=converted;
            }
            done+=amount;
        }
        return true;
    }
public:
    explicit WaveReader(std::unique_ptr<InputStream> input,const FileWorkCheck& check):input_(std::move(input)) {
        require(input_&&input_->getTotalLength()>=12,"Cannot read WAV header");
        std::array<std::uint8_t,40> bytes{};readAt(0,bytes.data(),12);
        const auto magic=std::uint32_t(integer(bytes.data(),4));big_=magic==tag("RIFX");
        const bool wide=magic==tag("RF64")||magic==tag("BW64");
        require((wide||magic==tag("RIFF")||big_)&&integer(bytes.data()+8,4)==tag("WAVE"),"Invalid WAV container");
        const auto total=std::uint64_t(input_->getTotalLength());
        std::uint64_t end=wide?total:integer(bytes.data()+4,4,big_)+8;
        require(end>=12&&end<=total,"Truncated WAV container");
        bool format{},hasDs64{};unsigned encoding{};
        std::uint64_t data64{},samples64{},tableOffset{},tableCount{};
        std::vector<std::uint64_t> usedSizes;
        std::vector<std::pair<std::uint64_t,std::uint64_t>> data;
        for(std::uint64_t position=12;position<end;) {
            detail::work(check);require(end-position>=8,"Truncated WAV chunk header");readAt(position,bytes.data(),8);
            const auto kind=std::uint32_t(integer(bytes.data(),4));std::uint64_t size=integer(bytes.data()+4,4,big_);const auto begin=position+8;
            if(size==UINT32_MAX) {
                require(wide&&hasDs64,"RF64 size has no ds64 metadata");
                if(kind==tag("data")&&data.empty())size=data64;
                else {
                    bool found{};std::array<std::uint8_t,12> entry{};
                    for(std::uint64_t n=0;n<tableCount;++n){detail::work(check);if(std::find(usedSizes.begin(),usedSizes.end(),n)!=usedSizes.end())continue;readAt(tableOffset+n*12,entry.data(),entry.size());if(integer(entry.data(),4)==kind){size=integer(entry.data()+4,8);usedSizes.push_back(n);found=true;break;}}
                    require(found,"RF64 chunk has no64-bit size entry");
                }
            }
            require(begin<=end&&size<=end-begin,"WAV chunk crosses its container");
            if(kind==tag("ds64")) {
                require(wide&&!hasDs64&&size>=28,"Invalid RF64 ds64 chunk");readAt(begin,bytes.data(),28);
                const auto root=integer(bytes.data(),8);require(root<=total-8&&root+8>=begin+size,"Invalid RF64 container size");end=root+8;
                data64=integer(bytes.data()+8,8);samples64=integer(bytes.data()+16,8);tableCount=integer(bytes.data()+24,4);require(tableCount<=(size-28)/12,"Truncated RF64 size table");tableOffset=begin+28;hasDs64=true;
            } else if(kind==tag("fmt ")) {
                require(!format&&size>=16,"Invalid or duplicate WAV format chunk");readAt(begin,bytes.data(),std::size_t(std::min<std::uint64_t>(size,40)));
                encoding=unsigned(integer(bytes.data(),2,big_));info_.channels=unsigned(integer(bytes.data()+2,2,big_));info_.sampleRate=double(integer(bytes.data()+4,4,big_));frameBytes_=unsigned(integer(bytes.data()+12,2,big_));info_.bitsPerSample=unsigned(integer(bytes.data()+14,2,big_));info_.validBitsPerSample=info_.bitsPerSample;
                if(encoding==0xfffe) {
                    if(big_)throw detail::UnsupportedWaveEncoding("RIFX extensible codec requires native provider");
                    require(size>=40&&integer(bytes.data()+16,2)>=22,"Truncated extensible WAV format");
                    info_.validBitsPerSample=unsigned(integer(bytes.data()+18,2));info_.channelMask=std::uint32_t(integer(bytes.data()+20,4));
                    constexpr std::uint8_t guidTail[]{0,0,0x10,0,0x80,0,0,0xaa,0,0x38,0x9b,0x71};
                    if(std::memcmp(bytes.data()+28,guidTail,sizeof(guidTail))!=0)throw detail::UnsupportedWaveEncoding("Unsupported WAV subtype");
                    encoding=unsigned(integer(bytes.data()+24,4));
                }
                if(encoding!=1&&encoding!=3)throw detail::UnsupportedWaveEncoding("Compressed WAV codec requires native provider");
                info_.floatingPoint=encoding==3;sampleBytes_=(info_.bitsPerSample+7)/8;
                require(info_.channels&&info_.sampleRate>0&&sampleBytes_&&std::uint64_t(sampleBytes_)*info_.channels<=frameBytes_,"Invalid WAV sample frame");
                require((encoding==1&&(sampleBytes_==1||sampleBytes_==2||sampleBytes_==3||sampleBytes_==4))||(encoding==3&&(info_.bitsPerSample==32||info_.bitsPerSample==64)),"Unsupported PCM/float WAV precision");
                if(!info_.validBitsPerSample)info_.validBitsPerSample=info_.bitsPerSample;
                require(info_.validBitsPerSample<=sampleBytes_*8,"Invalid WAV valid-bit count");format=true;
            } else if(kind==tag("bext")&&size>=346) {
                readAt(begin+338,bytes.data(),8);info_.timeReferenceSamples=integer(bytes.data(),8,big_);
            } else if(kind==tag("data"))data.push_back({begin,size});
            position=begin+size;if((size&1)&&position<end)++position;
        }
        require(format&&!data.empty()&&(!wide||hasDs64),"WAV is missing format,audio or64-bit metadata");
        for(const auto& part:data) {
            const auto frames=part.second/frameBytes_;require(frames<=std::uint64_t(INT64_MAX-info_.frames),"WAV frames exceed source addressing");
            if(frames)segments_.push_back({info_.frames,std::int64_t(frames),part.first});info_.frames+=std::int64_t(frames);
        }
        require(!samples64||samples64==std::uint64_t(info_.frames),"RF64 sample count disagrees with its audio bytes");
        info_.format=wide?"RF64":big_?"RIFX":"WAV";
    }
    const AudioFileInfo& info()const noexcept override{return info_;}
    bool read(float*const* p,std::size_t c,std::int64_t f,int n,const FileWorkCheck& check)override{return readImpl(p,c,f,n,check);}
    bool read(double*const* p,std::size_t c,std::int64_t f,int n,const FileWorkCheck& check)override{return readImpl(p,c,f,n,check);}
    Result status()const override{const std::lock_guard lock(mutex_);return status_;}
    std::size_t workingMemoryBytes()const override{const std::lock_guard lock(mutex_);return buffer_.capacity()+active_.capacity()*sizeof(std::size_t);}
};
class WaveWriter final:public AudioFileWriter {
    std::unique_ptr<FileOutputStream> output_;WaveFileOptions options_;unsigned sampleBytes_{},frameBytes_{};
    ByteBlock header_;std::vector<std::uint8_t> buffer_;std::int64_t frames_{};bool finished_{};Result status_=Result::ok();
    bool fail(const char* message){status_=Result::fail(message);return false;}
    template<class Value>bool writeImpl(const Value*const* input,std::size_t channels,int count,const FileWorkCheck& check)try {
        if(finished_||status_.failed())return false;
        if((count&&!input)||channels!=options_.channels||count<0||std::int64_t(count)>INT64_MAX-frames_)return fail("Invalid WAV output block");
        if(!count)return true;
        for(std::size_t c=0;c<channels;++c)if(!input[c])return fail("Missing WAV output channel");
        const auto maximum=std::max<std::size_t>(1,transferBytes/frameBytes_);
        for(int first=0;first<count;) {
            detail::work(check);const auto amount=int(std::min<std::size_t>(std::size_t(count-first),maximum));buffer_.resize(std::size_t(amount)*frameBytes_);
            for(int n=0;n<amount;++n)for(std::size_t c=0;c<channels;++c) {
                const double value=double(input[c][first+n]);if(!std::isfinite(value))return fail("Non-finite WAV output sample");
                auto* destination=buffer_.data()+std::size_t(n)*frameBytes_+c*sampleBytes_;
                if(options_.floatingPoint) {
                    if(sampleBytes_==4){const auto f=float(value);if(!std::isfinite(f))return fail("WAV sample exceeds float32 range");put(destination,std::bit_cast<std::uint32_t>(f),4);}
                    else put(destination,std::bit_cast<std::uint64_t>(value),8);
                } else {
                    const auto scale=std::ldexp(1.,int(options_.bitsPerSample)-1);
                    const auto quantized=std::llround(std::clamp(value,-1.,1.-1./scale)*scale);
                    if(sampleBytes_==1)*destination=std::uint8_t(quantized+128);
                    else put(destination,std::uint64_t(quantized),sampleBytes_);
                }
            }
            if(!output_->write(buffer_.data(),buffer_.size())){status_=output_->getStatus();return false;}
            frames_+=amount;first+=amount;
        }
        return true;
    }catch(...){status_=Result::fail("WAV output was interrupted");throw;}
public:
    WaveWriter(std::unique_ptr<FileOutputStream> output,const WaveFileOptions& options):output_(std::move(output)),options_(options) {
        sampleBytes_=(options.bitsPerSample+7)/8;frameBytes_=sampleBytes_*options.channels;
        header_=waveFileHeader(options_,0);
        require(output_->write(header_.getData(),header_.getSize()),"Cannot write WAV header");
    }

    bool write(const float*const* p,std::size_t c,int n,const FileWorkCheck& check)override{return writeImpl(p,c,n,check);}
    bool write(const double*const* p,std::size_t c,int n,const FileWorkCheck& check)override{return writeImpl(p,c,n,check);}
    Result finish()override {
        if(finished_||status_.failed())return status_;
        const auto bytes=std::uint64_t(frames_)*frameBytes_;
        if(bytes&1){const std::uint8_t zero{};if(!output_->write(&zero,1)){status_=output_->getStatus();return status_;}}
        const auto length=output_->getPosition();
        header_=waveFileHeader(options_,frames_);
        if(!output_->setPosition(0)||!output_->write(header_.getData(),header_.getSize())||!output_->setPosition(length)){status_=output_->getStatus();if(status_.wasOk())status_=Result::fail("Cannot finalize WAV header");return status_;}
        output_->flush();status_=output_->getStatus();if(status_.wasOk())finished_=true;return status_;
    }
    Result status()const override{return status_;}
    std::int64_t framesWritten()const noexcept override{return frames_;}
};
}
namespace detail {
std::shared_ptr<AudioFileReader> openWaveFile(const File& file,const FileWorkCheck& check){
    auto input = file.createInputStream();
    require(bool(input), "Cannot read WAV header");
    return openWaveStream(std::move(input), check);
}
}
ByteBlock waveFileHeader(const WaveFileOptions& options,std::int64_t frames) {
    const bool precision=options.floatingPoint?(options.bitsPerSample==32||options.bitsPerSample==64):(options.bitsPerSample==8||options.bitsPerSample==16||options.bitsPerSample==24||options.bitsPerSample==32);
    require(precision&&std::isfinite(options.sampleRate)&&options.sampleRate>=1&&options.sampleRate<=UINT32_MAX&&std::floor(options.sampleRate)==options.sampleRate&&options.channels&&options.channels<=UINT16_MAX,"Invalid WAV output format");
    const auto frame=std::uint64_t(options.channels)*((options.bitsPerSample+7)/8);
    require(frame<=UINT16_MAX&&frame*options.sampleRate<=UINT32_MAX,"WAV sample frame or byte rate exceeds its format field");
    require(frames>=0,"Negative WAV frame count");
    std::vector<std::uint8_t> header;
    const auto appendTag=[&](const char* name){header.insert(header.end(),name,name+4);};
    const auto field=[&](std::uint64_t value,unsigned count){const auto at=header.size();header.resize(at+count);put(header.data()+at,value,count);};
    appendTag("RIFF");field(0,4);appendTag("WAVE");appendTag("JUNK");field(28,4);header.resize(48);
    if(options.timeReferenceSamples){appendTag("bext");field(602,4);const auto begin=header.size();header.resize(begin+602);put(header.data()+begin+338,*options.timeReferenceSamples,8);put(header.data()+begin+346,1,2);}
    const bool extended=options.channels>2;appendTag("fmt ");field(extended?40:16,4);
    field(extended?0xfffe:options.floatingPoint?3:1,2);field(options.channels,2);field(std::uint32_t(options.sampleRate),4);field(frame*std::uint32_t(options.sampleRate),4);field(frame,2);field(options.bitsPerSample,2);
    if(extended){field(22,2);field(options.bitsPerSample,2);field(options.channelMask,4);field(options.floatingPoint?3:1,4);constexpr std::uint8_t tail[]{0,0,0x10,0,0x80,0,0,0xaa,0,0x38,0x9b,0x71};header.insert(header.end(),std::begin(tail),std::end(tail));}
    std::size_t fact{};if(options.floatingPoint){appendTag("fact");field(4,4);fact=header.size();field(0,4);}
    appendTag("data");field(0,4);
    require(std::uint64_t(frames)<=(std::uint64_t(INT64_MAX)-header.size()-1)/frame,"WAV output exceeds signed file addressing");
    const auto bytes=std::uint64_t(frames)*frame,length=header.size()+bytes+(bytes&1);
    const bool wide=length-8>UINT32_MAX||bytes>UINT32_MAX;
    if(wide){std::memcpy(header.data(),"RF64",4);std::memcpy(header.data()+12,"ds64",4);put(header.data()+20,length-8,8);put(header.data()+28,bytes,8);put(header.data()+36,std::uint64_t(frames),8);}
    put(header.data()+4,wide?UINT32_MAX:length-8,4);put(header.data()+header.size()-4,wide?UINT32_MAX:bytes,4);
    if(fact)put(header.data()+fact,std::uint64_t(frames)>UINT32_MAX?UINT32_MAX:std::uint64_t(frames),4);
    return ByteBlock(header.data(),header.size());
}
std::shared_ptr<AudioFileReader> openWaveStream(std::unique_ptr<InputStream> input, const FileWorkCheck& check) {
    return std::make_shared<WaveReader>(std::move(input), check);
}

std::unique_ptr<AudioFileWriter> createWaveFile(const File& file,const WaveFileOptions& options) {
    (void)waveFileHeader(options,0); // Validate before creating any destination.
    auto output=file.createOutputStream();require(bool(output),"Cannot exclusively create WAV output");
    struct Guard{File file;bool complete{};~Guard(){if(!complete)file.deleteFile();}}guard{file};
    auto writer=std::make_unique<WaveWriter>(std::move(output),options);guard.complete=true;return writer;
}
}
