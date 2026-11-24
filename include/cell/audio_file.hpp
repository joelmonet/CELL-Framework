// Original Cell Framework implementation; see LICENSE.
#pragma once
#include <cell/file.hpp>
#include <functional>
#include <optional>

namespace cell::audio {
using FileWorkCheck = std::function<void()>;
struct AudioFileInfo {
    unsigned channels{}, bitsPerSample{}, validBitsPerSample{};
    std::int64_t frames{};
    double sampleRate{};
    bool floatingPoint{};
    std::uint32_t channelMask{};
    std::optional<std::uint64_t> timeReferenceSamples;
    Text format;
};
// Control/worker-thread API. Implementations serialize their file cursor.
// Null destination channels are skipped; extra destination channels are zeroed.
// Frame addressing never narrows to a 32-bit file position.
class AudioFileReader {
public:
    virtual ~AudioFileReader() = default;
    virtual const AudioFileInfo& info() const noexcept = 0;
    virtual bool read(float* const*, std::size_t channels, std::int64_t first, int frames,
                      const FileWorkCheck& = {}) = 0;
    virtual bool read(double* const*, std::size_t channels, std::int64_t first, int frames,
                      const FileWorkCheck& = {}) = 0;
    virtual Result status() const = 0;
    virtual std::size_t workingMemoryBytes() const = 0;
};
struct WaveFileOptions {
    double sampleRate = 48000;
    unsigned channels = 2, bitsPerSample = 32;
    bool floatingPoint = true;
    std::uint32_t channelMask{};
    std::optional<std::uint64_t> timeReferenceSamples;
};
class AudioFileWriter {
public:
    virtual ~AudioFileWriter() = default;
    virtual bool write(const float* const*, std::size_t channels, int frames, const FileWorkCheck& = {}) = 0;
    virtual bool write(const double* const*, std::size_t channels, int frames, const FileWorkCheck& = {}) = 0;
    virtual Result finish() = 0;
    virtual Result status() const = 0;
    virtual std::int64_t framesWritten() const noexcept = 0;
};
// Native PCM/IEEE WAV,RIFX,RF64/BW64. Other supported formats use the separately
// licensed native codec provider, never an external framework audio object.
std::shared_ptr<AudioFileReader> openAudioFile(const File&, const FileWorkCheck& = {});
// Decode native WAVE from an owned, seekable stream whose range starts at the
// WAVE header. Stream positions and length must describe that range only.
std::shared_ptr<AudioFileReader> openWaveStream(std::unique_ptr<InputStream>, const FileWorkCheck& = {});
// A finalized header for known-length PCM/float interchange or sparse captures.
// The returned size is the first audio-byte offset; callers include an odd-byte
// pad after the data when required. This uses the same layout as the writer.
ByteBlock waveFileHeader(const WaveFileOptions&, std::int64_t frames);
std::unique_ptr<AudioFileWriter> createWaveFile(const File&, const WaveFileOptions&);
std::unique_ptr<AudioFileWriter> createFlacFile(const File&, double rate, unsigned channels,
                                               unsigned bits, int compression = 5);
bool compressedAudioCodecsAvailable() noexcept;
Text compressedAudioCodecVersion();
}
