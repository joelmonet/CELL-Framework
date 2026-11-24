// Original Cell Framework implementation; see LICENSE.
#include "audio_file_internal.hpp"
#include <array>
#include <cstring>
namespace cell::audio {
std::shared_ptr<AudioFileReader> openAudioFile(const File& file, const FileWorkCheck& check) {
    detail::work(check);
    std::array<char,12> header{};
    {
        auto input=file.createInputStream();
        detail::require(bool(input),"Cannot open audio input");
        if(input->read(header.data(),int(header.size()))!=int(header.size()))throw std::runtime_error("Audio input is too short");
    }
    if((std::memcmp(header.data(),"RIFF",4)==0||std::memcmp(header.data(),"RIFX",4)==0||
        std::memcmp(header.data(),"RF64",4)==0||std::memcmp(header.data(),"BW64",4)==0)&&std::memcmp(header.data()+8,"WAVE",4)==0) {
        try{return detail::openWaveFile(file,check);}
        catch(const detail::UnsupportedWaveEncoding&){} // Native provider handles compressed WAV variants.
    }
    return detail::openCodecFile(file,check);
}
std::unique_ptr<AudioFileWriter> createFlacFile(const File& file,double rate,unsigned channels,unsigned bits,int compression) {
    return detail::createCodecFlac(file,rate,channels,bits,compression);
}
bool compressedAudioCodecsAvailable() noexcept{return detail::codecAvailable();}
Text compressedAudioCodecVersion(){return detail::codecVersion();}
}
