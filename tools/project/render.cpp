// Original Cell Framework implementation; see LICENSE.
#include "gain.hpp"
#include <cell/audio_file.hpp>
#include <cell/files.hpp>
#include <cell/uuid.hpp>
#include <cell/json.hpp>
#include <iostream>

int main(int argc, char** argv) {
    using namespace cell;
    try {
        if (argc != 4)
            throw std::invalid_argument("Usage: <project> input.wav output.wav gain-dB");
        std::string_view gainText = argv[3];
        if (gainText.starts_with('+'))
            gainText.remove_prefix(1);
        const auto value = json::read(gainText);
        const auto* gain = value.number();
        if (!gain || *gain < -60 || *gain > 12)
            throw std::invalid_argument("Gain must be between -60 and +12 dB");

        const auto input = audio::openAudioFile(File(argv[1]));
        const auto& info = input->info();
        if (info.channels != 1 && info.channels != 2)
            throw std::invalid_argument("The reference processor accepts mono or stereo input");

        constexpr unsigned blockFrames = 512;
        Engine engine(plugin::descriptor(), plugin::createProcessor());
        engine.parameters().set(example::gainId, *gain);
        engine.prepare(info.sampleRate, blockFrames, info.channels);
        AudioBuffer<float> buffer(info.channels, blockFrames);
        const File destination(argv[2]);
        if (destination.exists())
            throw std::invalid_argument("Choose a new output path");
        const auto temporary = destination.getSiblingFile(".cell-render-" + Uuid().toString());
        struct Cleanup {
            const File& path;
            bool active{};
            ~Cleanup() { if (active) path.deleteFile(); }
        } cleanup{temporary};
        auto output = audio::createWaveFile(temporary,
            {info.sampleRate, info.channels, 32, true, info.channelMask, info.timeReferenceSamples});
        cleanup.active = true;
        for (std::int64_t frame = 0; frame < info.frames;) {
            const auto count = static_cast<int>(std::min<std::int64_t>(blockFrames, info.frames - frame));
            if (!input->read(buffer.getArrayOfWritePointers(), info.channels, frame, count))
                throw std::runtime_error(input->status().getErrorMessage().toStdString());
            engine.process(buffer.block().slice(0, static_cast<unsigned>(count)));
            if (!output->write(buffer.getArrayOfReadPointers(), info.channels, count))
                throw std::runtime_error(output->status().getErrorMessage().toStdString());
            frame += count;
        }
        const auto finished = output->finish();
        if (!finished)
            throw std::runtime_error(finished.getErrorMessage().toStdString());
        output.reset();
        const auto published = files::publish(temporary.nativePath(), destination.nativePath(), false);
        if (!published)
            throw std::runtime_error(published.getErrorMessage().toStdString());
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
