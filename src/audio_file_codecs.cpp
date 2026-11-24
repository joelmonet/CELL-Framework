// Original Cell Framework implementation; see LICENSE.
// Native libsndfile C ABI; see third_party/sndfile/COPYING.
#include "audio_file_internal.hpp"
#include "../third_party/sndfile/sndfile.h"
#include <algorithm>
#include <cmath>
#include <cstring>
#include <exception>
#include <mutex>
#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#else
#include <dlfcn.h>
#endif

namespace cell::audio::detail {
namespace {
struct Library {
#ifdef _WIN32
    HMODULE handle{};
    bool open(const std::filesystem::path& path){handle=LoadLibraryExW(path.c_str(),nullptr,LOAD_LIBRARY_SEARCH_DEFAULT_DIRS|(path.is_absolute()?LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR:0));return handle!=nullptr;}
    void* symbol(const char* name)const{return reinterpret_cast<void*>(GetProcAddress(handle,name));}
    ~Library(){if(handle)FreeLibrary(handle);}
#else
    void* handle{};
    bool open(const std::filesystem::path& path){handle=dlopen(path.c_str(),RTLD_NOW|RTLD_LOCAL);return handle!=nullptr;}
    void* symbol(const char* name)const{return dlsym(handle,name);}
    ~Library(){if(handle)dlclose(handle);}
#endif
};
struct Api {
    Library library;
    decltype(&sf_open_virtual) open{};decltype(&sf_close) close{};
    decltype(&sf_readf_float) readFloat{};decltype(&sf_readf_double) readDouble{};
    decltype(&sf_writef_int) writeInt{};
    decltype(&sf_seek) seek{};decltype(&sf_command) command{};
    decltype(&sf_strerror) error{};decltype(&sf_error) errorCode{};
    decltype(&sf_version_string) version{};decltype(&sf_format_check) formatCheck{};
    template<class T>void resolve(T& destination,const char* name){destination=reinterpret_cast<T>(library.symbol(name));require(destination!=nullptr,"Native audio codec provider has an incompatible API");}
    Api() {
#ifdef _WIN32
        const std::array<const char*,2> names{"libsndfile-1.dll","sndfile.dll"};
#elif defined(__APPLE__)
        const std::array<const char*,2> names{"libsndfile.1.dylib","libsndfile.dylib"};
#else
        const std::array<const char*,2> names{"libsndfile.so.1","libsndfile.so"};
#endif
        const auto directory=File::getSpecialLocation(File::currentModuleFile).getParentDirectory();bool loaded{};
        for(const auto* name:names)if(library.open(directory.getChildFile(name).nativePath())||library.open(std::filesystem::path(name))){loaded=true;break;}
        require(loaded,"Native compressed audio codec provider is unavailable");
        resolve(open,"sf_open_virtual");resolve(close,"sf_close");resolve(readFloat,"sf_readf_float");resolve(readDouble,"sf_readf_double");resolve(writeInt,"sf_writef_int");resolve(seek,"sf_seek");resolve(command,"sf_command");resolve(error,"sf_strerror");resolve(errorCode,"sf_error");resolve(version,"sf_version_string");resolve(formatCheck,"sf_format_check");
    }
};
std::shared_ptr<Api> provider(){static const auto api=std::make_shared<Api>();return api;}
struct Io {
    std::unique_ptr<FileInputStream> input;std::unique_ptr<FileOutputStream> output;
    sf_count_t length{};bool failed{};const FileWorkCheck* activeCheck{};std::exception_ptr pending;
    struct Operation {
        Io& io;
        Operation(Io& value,const FileWorkCheck& check):io(value){io.activeCheck=&check;io.pending=nullptr;}
        ~Operation(){io.activeCheck=nullptr;}
    };
    bool checkpoint()noexcept {
        if(pending)return false;
        try{if(activeCheck)work(*activeCheck);return true;}catch(...){pending=std::current_exception();return false;}
    }
    void rethrow(){if(pending)std::rethrow_exception(pending);}
    sf_count_t position()const{return input?input->getPosition():output?output->getPosition():0;}
    static sf_count_t size(void* opaque)noexcept{auto& io=*static_cast<Io*>(opaque);return io.input?io.input->getTotalLength():io.length;}
    static sf_count_t tell(void* opaque)noexcept{try{return static_cast<Io*>(opaque)->position();}catch(...){static_cast<Io*>(opaque)->failed=true;return -1;}}
    static sf_count_t seek(sf_count_t offset,int whence,void* opaque)noexcept{
        auto& io=*static_cast<Io*>(opaque);try{
            if(!io.checkpoint())return -1;
            sf_count_t base{};if(whence==SEEK_CUR)base=io.position();else if(whence==SEEK_END)base=size(opaque);else if(whence!=SEEK_SET)return -1;
            if(base<0||(offset>0&&base>INT64_MAX-offset)||(offset<0&&offset<-base))return -1;
            const auto target=base+offset;
            if((io.input&&!io.input->setPosition(target))||(io.output&&!io.output->setPosition(target))){io.failed=true;return -1;}return target;
        }catch(...){io.failed=true;return -1;}
    }
    static sf_count_t read(void* data,sf_count_t count,void* opaque)noexcept{
        auto& io=*static_cast<Io*>(opaque);try{
            if(!io.input||count<0||std::uint64_t(count)>SIZE_MAX||(!data&&count))return 0;sf_count_t at{};
            while(at<count){if(!io.checkpoint())return at;const auto amount=int(std::min<sf_count_t>(count-at,65536));const auto got=io.input->read(static_cast<std::byte*>(data)+std::size_t(at),amount);if(got<=0)break;at+=got;}
            if(io.input->getStatus().failed())io.failed=true;return at;
        }catch(...){io.failed=true;return 0;}
    }
    static sf_count_t write(const void* data,sf_count_t count,void* opaque)noexcept{
        auto& io=*static_cast<Io*>(opaque);try{
            if(!io.output||count<0||std::uint64_t(count)>SIZE_MAX||(!data&&count))return 0;
            sf_count_t at{};while(at<count){if(!io.checkpoint())return at;const auto amount=std::size_t(std::min<sf_count_t>(65536,count-at));if(!io.output->write(static_cast<const std::byte*>(data)+std::size_t(at),amount)){io.failed=true;return at;}at+=sf_count_t(amount);}
            io.length=std::max(io.length,io.output->getPosition());return count;
        }catch(...){io.failed=true;return 0;}
    }
    static SF_VIRTUAL_IO callbacks(){return {size,seek,read,write,tell};}
};
unsigned bitDepth(int format) {
    switch(format&SF_FORMAT_SUBMASK){case SF_FORMAT_PCM_U8:case SF_FORMAT_PCM_S8:return 8;case SF_FORMAT_PCM_16:return 16;case SF_FORMAT_PCM_24:return 24;case SF_FORMAT_PCM_32:case SF_FORMAT_FLOAT:return 32;case SF_FORMAT_DOUBLE:return 64;default:return 0;}
}
Text formatName(int format) {
    switch(format&SF_FORMAT_TYPEMASK){case SF_FORMAT_FLAC:return "FLAC";case SF_FORMAT_AIFF:return "AIFF";case SF_FORMAT_OGG:return "Ogg";case SF_FORMAT_MPEG:return "MPEG audio";case SF_FORMAT_W64:return "Wave64";case SF_FORMAT_WAV:case SF_FORMAT_WAVEX:return "WAV codec";case SF_FORMAT_RF64:return "RF64 codec";default:return "Native audio codec";}
}
class CodecReader final:public AudioFileReader {
    std::shared_ptr<Api> api_=provider();Io io_;SNDFILE* native_{};AudioFileInfo info_;
    std::vector<float> floats_;std::vector<double> doubles_;mutable std::mutex mutex_;Result status_=Result::ok();
    bool restart_{};
    void restart() {
        if(native_){api_->close(native_);native_=nullptr;}
        io_.failed=false;require(io_.input->setPosition(0),"Cannot restart the audio source reader");
        auto callbacks=Io::callbacks();SF_INFO metadata{};native_=api_->open(&callbacks,SFM_READ,&metadata,&io_);
        io_.rethrow();require(native_!=nullptr,"Cannot restart the audio decoder after cancellation");
        require(metadata.frames==info_.frames&&metadata.channels==int(info_.channels)&&metadata.samplerate==int(info_.sampleRate),"Audio metadata changed during decoder recovery");
        restart_=false;
    }
    template<class Value>bool readImpl(Value*const* outputs,std::size_t channels,std::int64_t first,int frames,const FileWorkCheck& check){
        const std::lock_guard lock(mutex_);status_=Result::ok();
        if((channels&&!outputs)||first<0||frames<0||first>info_.frames||std::int64_t(frames)>info_.frames-first){status_=Result::fail("Invalid audio codec read range");return false;}
        for(std::size_t c=info_.channels;c<channels;++c)if(outputs[c])std::fill_n(outputs[c],frames,Value{});
        if(!frames)return true;
        Io::Operation operation(io_,check);
        struct CancellationState {bool& restart;Io& io;~CancellationState(){if(io.pending)restart=true;}} cancellationState{restart_,io_};
        if(restart_)restart();
        work(check);const auto seek=api_->seek(native_,first,SEEK_SET);io_.rethrow();if(seek!=first){status_=Result::fail("Cannot seek native audio codec");return false;}
        auto& buffer=[&]() -> auto& {if constexpr(std::is_same_v<Value,float>)return floats_;else return doubles_;}();
        const auto maximum=std::max<std::size_t>(1,65536/info_.channels);
        for(int offset=0;offset<frames;){
            work(check);const auto count=int(std::min<std::size_t>(std::size_t(frames-offset),maximum));
            require(std::uint64_t(count)*info_.channels<=SIZE_MAX/sizeof(Value),"Codec sample block exceeds address space");
            buffer.resize(std::size_t(count)*info_.channels);
            const auto got=[&](){if constexpr(std::is_same_v<Value,float>)return api_->readFloat(native_,buffer.data(),count);else return api_->readDouble(native_,buffer.data(),count);}();
            io_.rethrow();
            if(got!=count||io_.failed||api_->errorCode(native_)){status_=Result::fail(Text("Native audio read failed: ")+Text(api_->error(native_)));return false;}
            for(std::size_t c=0;c<std::min<std::size_t>(channels,info_.channels);++c)if(outputs[c])for(int n=0;n<count;++n){const auto value=buffer[std::size_t(n)*info_.channels+c];if(!std::isfinite(value)){status_=Result::fail("Non-finite decoded audio sample");return false;}outputs[c][offset+n]=value;}
            offset+=count;
        }return true;
    }
public:
    CodecReader(const File& file,const FileWorkCheck& check){
        work(check);io_.input=file.createInputStream();require(bool(io_.input),"Cannot open native audio input");auto callbacks=Io::callbacks();SF_INFO metadata{};Io::Operation operation(io_,check);native_=api_->open(&callbacks,SFM_READ,&metadata,&io_);
        if(!native_){io_.rethrow();throw std::runtime_error(std::string("Cannot decode audio: ")+api_->error(nullptr));}
        try{
            io_.rethrow();
            require(metadata.frames>=0&&metadata.channels>0&&metadata.samplerate>0,"Invalid native codec metadata");
            info_.frames=metadata.frames;info_.channels=unsigned(metadata.channels);info_.sampleRate=metadata.samplerate;info_.bitsPerSample=info_.validBitsPerSample=bitDepth(metadata.format);const auto sub=metadata.format&SF_FORMAT_SUBMASK;info_.floatingPoint=sub==SF_FORMAT_FLOAT||sub==SF_FORMAT_DOUBLE;info_.format=formatName(metadata.format);
            SF_BROADCAST_INFO broadcast{};if(api_->command(native_,SFC_GET_BROADCAST_INFO,&broadcast,sizeof(broadcast))==SF_TRUE)info_.timeReferenceSamples=std::uint64_t(broadcast.time_reference_low)|(std::uint64_t(broadcast.time_reference_high)<<32);
        }catch(...){api_->close(native_);native_=nullptr;throw;}
    }
    ~CodecReader(){if(native_)api_->close(native_);}
    const AudioFileInfo& info()const noexcept override{return info_;}
    bool read(float*const* p,std::size_t c,std::int64_t f,int n,const FileWorkCheck& check)override{return readImpl(p,c,f,n,check);}
    bool read(double*const* p,std::size_t c,std::int64_t f,int n,const FileWorkCheck& check)override{return readImpl(p,c,f,n,check);}
    Result status()const override{const std::lock_guard lock(mutex_);return status_;}
    std::size_t workingMemoryBytes()const override{const std::lock_guard lock(mutex_);return floats_.capacity()*sizeof(float)+doubles_.capacity()*sizeof(double);}
};
class CodecWriter final:public AudioFileWriter {
    std::shared_ptr<Api> api_;Io io_;SNDFILE* native_{};unsigned channels_{},bits_{};std::int64_t frames_{};
    std::vector<int> integers_;Result status_=Result::ok();bool finished_{};
    template<class Value>bool writeImpl(const Value*const* inputs,std::size_t channels,int frames,const FileWorkCheck& check)try{
        if(finished_||status_.failed())return false;
        if((frames&&!inputs)||channels!=channels_||frames<0||std::int64_t(frames)>INT64_MAX-frames_){status_=Result::fail("Invalid native codec output block");return false;}
        if(!frames)return true;
        for(std::size_t c=0;c<channels;++c)if(!inputs[c]){status_=Result::fail("Missing audio output channel");return false;}
        Io::Operation operation(io_,check);
        const auto maximum=std::max<std::size_t>(1,65536/channels_);const auto scale=std::ldexp(1.,int(bits_)-1),top=1.-1./scale;
        for(int offset=0;offset<frames;){
            work(check);const auto count=int(std::min<std::size_t>(std::size_t(frames-offset),maximum));require(std::uint64_t(count)*channels_<=SIZE_MAX/sizeof(int),"Codec sample block exceeds address space");integers_.resize(std::size_t(count)*channels_);
            static_assert(sizeof(int)==4);
            for(int n=0;n<count;++n)for(std::size_t c=0;c<channels;++c){const auto value=double(inputs[c][offset+n]);if(!std::isfinite(value)){status_=Result::fail("Non-finite codec output sample");return false;}const auto quantized=std::llround(std::clamp(value,-1.,top)*scale);const auto aligned=std::uint32_t(std::uint64_t(quantized)<<(32-bits_));integers_[std::size_t(n)*channels+c]=std::bit_cast<std::int32_t>(aligned);}
            const auto got=api_->writeInt(native_,integers_.data(),count);
            io_.rethrow();
            if(got!=count||io_.failed||api_->errorCode(native_)){status_=Result::fail(Text("Native audio encoding failed: ")+Text(api_->error(native_)));return false;}
            frames_+=count;offset+=count;
        }return true;
    }catch(...){status_=Result::fail("Native audio output was interrupted");throw;}
public:
    CodecWriter(std::shared_ptr<Api> api,std::unique_ptr<FileOutputStream> output,SF_INFO metadata,unsigned bits,int compression):api_(std::move(api)),channels_(unsigned(metadata.channels)),bits_(bits){
        io_.output=std::move(output);auto callbacks=Io::callbacks();native_=api_->open(&callbacks,SFM_WRITE,&metadata,&io_);if(!native_)throw std::runtime_error(std::string("Cannot initialize native FLAC encoder: ")+api_->error(nullptr));
        double level=compression/8.;if(api_->command(native_,SFC_SET_COMPRESSION_LEVEL,&level,sizeof(level))!=SF_TRUE){api_->close(native_);native_=nullptr;throw std::runtime_error("Native FLAC encoder rejected compression settings");}
    }
    ~CodecWriter(){if(native_)api_->close(native_);}
    bool write(const float*const* p,std::size_t c,int n,const FileWorkCheck& check)override{return writeImpl(p,c,n,check);}
    bool write(const double*const* p,std::size_t c,int n,const FileWorkCheck& check)override{return writeImpl(p,c,n,check);}
    Result finish()override{
        if(finished_)return status_;
        if(native_){const auto result=api_->close(native_);native_=nullptr;if(result||io_.failed)status_=Result::fail("Cannot finalize native FLAC stream");}
        io_.output->flush();if(io_.output->getStatus().failed())status_=io_.output->getStatus();finished_=true;return status_;
    }
    Result status()const override{return status_;}
    std::int64_t framesWritten()const noexcept override{return frames_;}
};
}
std::shared_ptr<AudioFileReader> openCodecFile(const File& file,const FileWorkCheck& check){return std::make_shared<CodecReader>(file,check);}
std::unique_ptr<AudioFileWriter> createCodecFlac(const File& file,double rate,unsigned channels,unsigned bits,int compression){
    require(std::isfinite(rate)&&rate>=1&&rate<=INT32_MAX&&std::floor(rate)==rate&&channels&&channels<=INT32_MAX&&compression>=0&&compression<=8,"Invalid native FLAC settings");
    int sub{};switch(bits){case 8:sub=SF_FORMAT_PCM_S8;break;case 16:sub=SF_FORMAT_PCM_16;break;case 24:sub=SF_FORMAT_PCM_24;break;case 32:sub=SF_FORMAT_PCM_32;break;default:throw std::runtime_error("Unsupported native FLAC precision");}
    auto api=provider();SF_INFO metadata{};metadata.channels=int(channels);metadata.samplerate=int(rate);metadata.format=SF_FORMAT_FLAC|sub;require(api->formatCheck(&metadata)==SF_TRUE,"FLAC settings exceed the codec format");
    auto output=file.createOutputStream();require(bool(output),"Cannot exclusively create FLAC output");
    struct Guard{File file;bool complete{};~Guard(){if(!complete)file.deleteFile();}}guard{file};
    auto writer=std::make_unique<CodecWriter>(std::move(api),std::move(output),metadata,bits,compression);guard.complete=true;return writer;
}
bool codecAvailable()noexcept{try{return provider()!=nullptr;}catch(...){return false;}}
Text codecVersion(){return Text(provider()->version());}
}
