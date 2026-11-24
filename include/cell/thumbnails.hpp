#pragma once

#include <cell/assets.hpp>
#include <cmath>

namespace cell::waveform {
struct AudioInfo { double sampleRate{}; std::uint64_t frames{}; unsigned channels{}; };
class Reader {
public:
    virtual ~Reader() = default;
    virtual AudioInfo info() const noexcept = 0;
    // Worker thread, bounded interleaved float destination; no realtime promise.
    virtual bool read(std::uint64_t firstFrame, unsigned frames, std::span<float>) = 0;
};
// Built-in RIFF/WAVE PCM 8/16/24/32 and IEEE float 32/64 reader. Other codecs
// plug into Cache through ReaderFactory without introducing product dependencies.
std::unique_ptr<Reader> openWave(const std::filesystem::path&);
struct Peak {
    float minimum{}, maximum{};
    double sumSquares{};
    std::uint64_t samples{};
    double rms() const noexcept { return samples ? std::sqrt(sumSquares / samples) : 0; }
};
struct Level { std::uint64_t framesPerPeak{}; std::vector<Peak> peaks; };
struct Thumbnail {
    AudioInfo audio;
    std::vector<Level> levels; // interleaved channels, successively doubled bins
    std::size_t bytes() const noexcept;
    // Chooses existing summaries with at least one bin per display pixel when
    // available. Painting never decodes or scans the original audio.
    const Level& levelFor(std::uint64_t visibleFrames, unsigned pixels) const;
};
class Cache {
public:
    using ReaderFactory = std::function<std::unique_ptr<Reader>(const std::filesystem::path&)>;
    struct Limits { std::size_t bytes = 64u * 1024 * 1024, thumbnailBytes = 8u * 1024 * 1024; unsigned entries = 128; };
    Cache(Limits, ReaderFactory = openWave, std::shared_ptr<jobs::Pool> = jobs::Pool::shared());
    Cache() : Cache(Limits{}) {}
    static std::shared_ptr<Cache> shared();
    ~Cache();
    jobs::Job request(std::filesystem::path, jobs::Token, bool refresh = false);
    void clear();
    std::size_t residentBytes() const;
private:
    struct Impl;
    std::shared_ptr<Impl> impl_;
    std::shared_ptr<jobs::Pool> pool_;
};
}
