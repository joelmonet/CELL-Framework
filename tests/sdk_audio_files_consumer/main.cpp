// Original Cell Framework implementation; see LICENSE.
// Independent installed-package consumer, including transitive static linkage.
#include <cell/audio_file.hpp>
#include <cell/uuid.hpp>
#include <array>
#include <iostream>
#include <stdexcept>

int main() {
    const auto root=cell::File::getSpecialLocation(cell::File::tempDirectory)
        .getChildFile("cell-installed-audio-"+cell::Uuid().toString());
    try {
        if(root.createDirectory().failed())throw std::runtime_error("Cannot create owned fixtures");
        const auto file=root.getChildFile(cell::Text::fromUTF8("capture-鼓.wav"));
        {
            cell::audio::WaveFileOptions format;
            format.channels=64;format.bitsPerSample=64;format.sampleRate=96000;
            format.timeReferenceSamples=UINT64_C(1)<<40;
            auto writer=cell::audio::createWaveFile(file,format);
            const std::array<double,5> values{-1.25,-.123456789012345,0.,.5,1.75};
            std::array<const double*,64> inputs{};inputs.fill(values.data());
            if(!writer->write(inputs.data(),inputs.size(),int(values.size()))||writer->finish().failed())
                throw std::runtime_error("Installed writer failed");
        }
        {
            const auto reader=cell::audio::openAudioFile(file);
            if(reader->info().channels!=64||reader->info().frames!=5||
               reader->info().timeReferenceSamples!=(UINT64_C(1)<<40))
                throw std::runtime_error("Installed reader changed capture metadata");
            std::array<double,3> channel{};std::array<double*,64> outputs{};outputs.back()=channel.data();
            if(!reader->read(outputs.data(),outputs.size(),1,3)||
               channel!=std::array<double,3>{-.123456789012345,0.,.5})
                throw std::runtime_error("Installed selective float64 reader changed samples");
        }
        if(!root.deleteRecursively())throw std::runtime_error("Cannot remove owned fixtures");
        std::cout<<"PASS independent installed Cell::AudioFiles: transitive dependencies,64-channel float64,BWF,Unicode and selective random read\n";
        return 0;
    }catch(const std::exception& error){root.deleteRecursively();std::cerr<<error.what()<<'\n';return 1;}
}
