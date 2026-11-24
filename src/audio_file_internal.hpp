// Original Cell Framework implementation; see LICENSE.
#pragma once
#include <cell/audio_file.hpp>
#include <stdexcept>
namespace cell::audio::detail {
struct UnsupportedWaveEncoding : std::runtime_error { using std::runtime_error::runtime_error; };
std::shared_ptr<AudioFileReader> openWaveFile(const File&, const FileWorkCheck&);
std::shared_ptr<AudioFileReader> openCodecFile(const File&, const FileWorkCheck&);
std::unique_ptr<AudioFileWriter> createCodecFlac(const File&, double, unsigned, unsigned, int);
bool codecAvailable() noexcept;
Text codecVersion();
inline void require(bool value, const char* message) { if (!value) throw std::runtime_error(message); }
inline void work(const FileWorkCheck& check) { if (check) check(); }
}
