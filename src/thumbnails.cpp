#include <cell/thumbnails.hpp>
#include "detail/integer_decimal.hpp"
#include <algorithm>
#include <array>
#include <bit>
#include <fstream>
#include <limits>
#include <mutex>
#include <stdexcept>

namespace cell::waveform {
namespace {
std::uint32_t u32(const unsigned char* p) { return p[0] | std::uint32_t(p[1]) << 8 | std::uint32_t(p[2]) << 16 | std::uint32_t(p[3]) << 24; }
unsigned u16(const unsigned char* p) { return p[0] | unsigned(p[1]) << 8; }
class WaveReader final : public Reader {
public:
    explicit WaveReader(const std::filesystem::path& path) : stream_(path, std::ios::binary) {
        if (!stream_) throw std::runtime_error("Could not open audio file");
        stream_.seekg(0, std::ios::end); const auto end = stream_.tellg();
        if (end < 12) throw std::runtime_error("Incomplete WAVE header");
        const auto size = std::uint64_t(end); stream_.seekg(0);
        std::array<unsigned char, 12> header{}; get(header.data(), header.size());
        if (u32(header.data()) != 0x46464952 || u32(header.data() + 8) != 0x45564157) throw std::runtime_error("Waveform preview requires a RIFF/WAVE file or a registered codec");
        const auto boundary = std::uint64_t(u32(header.data() + 4)) + 8;
        if (boundary < 12 || boundary > size) throw std::runtime_error("Invalid WAVE size");
        bool format = false, data = false;
        for (std::uint64_t at = 12; at < boundary;) {
            if (boundary - at < 8) throw std::runtime_error("Truncated WAVE chunk");
            stream_.seekg(std::streamoff(at)); std::array<unsigned char, 8> chunk{}; get(chunk.data(), chunk.size());
            const auto count = std::uint64_t(u32(chunk.data() + 4)), content = at + 8;
            if (count > boundary - content || (count & 1) > boundary - content - count) throw std::runtime_error("Invalid WAVE chunk size");
            if (u32(chunk.data()) == 0x20746d66) {
                if (format || count < 16 || count > 4096) throw std::runtime_error("Invalid WAVE format");
                std::vector<unsigned char> bytes(static_cast<std::size_t>(count)); get(bytes.data(), bytes.size());
                encoding_ = u16(bytes.data()); audio_.channels = u16(bytes.data() + 2); audio_.sampleRate = u32(bytes.data() + 4);
                alignment_ = u16(bytes.data() + 12); bits_ = u16(bytes.data() + 14);
                if (encoding_ == 0xfffe) {
                    static constexpr unsigned char tail[]{0, 0, 0x10, 0, 0x80, 0, 0, 0xaa, 0, 0x38, 0x9b, 0x71};
                    if (count < 40 || u16(bytes.data() + 16) < 22 || u16(bytes.data() + 18) != bits_ ||
                        !std::equal(std::begin(tail), std::end(tail), bytes.data() + 28)) throw std::runtime_error("Unsupported extensible WAVE format");
                    encoding_ = u32(bytes.data() + 24);
                }
                if (!audio_.channels || audio_.channels > 64 || audio_.sampleRate < 1000 || audio_.sampleRate > 768000 ||
                    (encoding_ == 1 ? (bits_ != 8 && bits_ != 16 && bits_ != 24 && bits_ != 32) : encoding_ != 3 || (bits_ != 32 && bits_ != 64)) ||
                    alignment_ != audio_.channels * (bits_ / 8) || u32(bytes.data() + 8) != unsigned(audio_.sampleRate) * alignment_) throw std::runtime_error("Unsupported WAVE sample format");
                format = true;
            } else if (u32(chunk.data()) == 0x61746164) {
                if (data) throw std::runtime_error("Multiple WAVE data chunks require a registered codec");
                dataOffset_ = content; dataBytes_ = count; data = true;
            }
            at = content + count + (count & 1);
        }
        if (!format || !data || dataBytes_ % alignment_) throw std::runtime_error("Invalid WAVE audio data");
        audio_.frames = dataBytes_ / alignment_;
    }
    AudioInfo info() const noexcept override { return audio_; }
    bool read(std::uint64_t first, unsigned count, std::span<float> output) override {
        if (count > 4096 || first > audio_.frames || count > audio_.frames - first || output.size() != std::size_t(count) * audio_.channels) return false;
        bytes_.resize(std::size_t(count) * alignment_);
        stream_.clear(); stream_.seekg(std::streamoff(dataOffset_ + first * alignment_));
        if (!stream_.read(reinterpret_cast<char*>(bytes_.data()), std::streamsize(bytes_.size()))) return false;
        for (std::size_t i = 0; i < output.size(); ++i) {
            const auto* p = bytes_.data() + i * (bits_ / 8); double value = 0;
            if (encoding_ == 3) {
                if (bits_ == 32) value = std::bit_cast<float>(u32(p));
                else value = std::bit_cast<double>(std::uint64_t(u32(p)) | std::uint64_t(u32(p + 4)) << 32);
            } else if (bits_ == 8) value = (int(p[0]) - 128) / 128.;
            else if (bits_ == 16) value = std::bit_cast<std::int16_t>(std::uint16_t(u16(p))) / 32768.;
            else if (bits_ == 24) { const int raw = p[0] | int(p[1]) << 8 | int(p[2]) << 16; value = (raw & 0x800000 ? raw - 0x1000000 : raw) / 8388608.; }
            else value = std::bit_cast<std::int32_t>(u32(p)) / 2147483648.;
            if (!std::isfinite(value) || std::abs(value) > std::numeric_limits<float>::max()) return false;
            output[i] = float(value);
        }
        return true;
    }
private:
    void get(unsigned char* data, std::size_t count) {
        if (!stream_.read(reinterpret_cast<char*>(data), std::streamsize(count))) throw std::runtime_error("Could not read WAVE header");
    }
    std::ifstream stream_; AudioInfo audio_; std::uint64_t dataOffset_{}, dataBytes_{};
    unsigned encoding_{}, alignment_{}, bits_{}; std::vector<unsigned char> bytes_;
};
std::string fingerprint(const std::filesystem::path& path) {
    std::error_code error; auto resolved = std::filesystem::weakly_canonical(path, error);
    if (error) throw std::runtime_error("Could not resolve audio file");
    const auto size = std::filesystem::file_size(resolved, error); if (error) throw std::runtime_error("Audio file is missing");
    const auto modified = std::filesystem::last_write_time(resolved, error); if (error) throw std::runtime_error("Could not inspect audio file");
    return assets::pathText(resolved) + '\n' + std::to_string(size) + '\n' + cell::detail::integerDecimal(modified.time_since_epoch().count());
}
void merge(Peak& destination, const Peak& source) {
    if (!source.samples) return;
    if (!destination.samples) destination = source;
    else { destination.minimum = std::min(destination.minimum, source.minimum); destination.maximum = std::max(destination.maximum, source.maximum); destination.sumSquares += source.sumSquares; destination.samples += source.samples; }
}
std::shared_ptr<const Thumbnail> build(Reader& reader, std::size_t budget, jobs::Context& context) {
    auto result = std::make_shared<Thumbnail>(); result->audio = reader.info(); const auto info = result->audio;
    if (!std::isfinite(info.sampleRate) || info.sampleRate < 1000 || info.sampleRate > 768000 || !info.channels || info.channels > 64 || !info.frames || info.frames > (std::uint64_t(1) << 52)) throw std::runtime_error("Invalid audio preview dimensions");
    std::uint64_t block = 256;
    const auto maximumBins = std::max<std::size_t>(1, (budget - 4096) / (sizeof(Peak) * info.channels * 3));
    while ((info.frames + block - 1) / block > maximumBins) block *= 2;
    const auto bins = std::size_t((info.frames + block - 1) / block);
    result->levels.push_back({block, std::vector<Peak>(bins * info.channels)});
    std::vector<float> buffer(4096 * info.channels);
    for (std::uint64_t offset = 0; offset < info.frames;) {
        if (context.cancelled()) return {};
        const auto count = unsigned(std::min<std::uint64_t>(4096, info.frames - offset));
        if (!reader.read(offset, count, {buffer.data(), std::size_t(count) * info.channels})) throw std::runtime_error("Could not decode audio preview");
        for (unsigned frame = 0; frame < count; ++frame) for (unsigned channel = 0; channel < info.channels; ++channel) {
            const auto value = buffer[std::size_t(frame) * info.channels + channel];
            if (!std::isfinite(value)) throw std::runtime_error("Audio preview contains non-finite samples");
            auto& peak = result->levels[0].peaks[std::size_t((offset + frame) / block) * info.channels + channel];
            merge(peak, {value, value, double(value) * value, 1});
        }
        offset += count; context.progress(double(offset) / info.frames * .9);
    }
    while (result->levels.back().peaks.size() > info.channels) {
        const auto& previous = result->levels.back(); const auto count = (previous.peaks.size() / info.channels + 1) / 2;
        Level next{previous.framesPerPeak * 2, std::vector<Peak>(count * info.channels)};
        for (std::size_t i = 0; i < previous.peaks.size() / info.channels; ++i) for (unsigned c = 0; c < info.channels; ++c) merge(next.peaks[(i / 2) * info.channels + c], previous.peaks[i * info.channels + c]);
        result->levels.push_back(std::move(next));
    }
    if (result->bytes() > budget) throw std::runtime_error("Waveform exceeds preview cache budget");
    return result;
}
}
std::unique_ptr<Reader> openWave(const std::filesystem::path& path) { return std::make_unique<WaveReader>(path); }
std::size_t Thumbnail::bytes() const noexcept {
    std::size_t count = sizeof(*this) + levels.size() * sizeof(Level);
    for (const auto& level : levels) count += level.peaks.size() * sizeof(Peak);
    return count;
}
const Level& Thumbnail::levelFor(std::uint64_t visible, unsigned pixels) const {
    if (levels.empty()) throw std::logic_error("empty thumbnail");
    const auto wanted = std::max<std::uint64_t>(1, visible / std::max(1u, pixels));
    const Level* selected = &levels.front();
    for (const auto& level : levels) { if (level.framesPerPeak > wanted) break; selected = &level; }
    return *selected;
}
struct Cache::Impl {
    struct Entry { std::string key; std::shared_ptr<const Thumbnail> thumbnail; std::uint64_t used{}; };
    Limits limits; ReaderFactory factory; mutable std::mutex mutex; std::vector<Entry> entries;
    std::size_t bytes{}; std::uint64_t clock{}, generation{};
    Impl(Limits options, ReaderFactory reader) : limits(options), factory(std::move(reader)) {}
};
Cache::Cache(Limits limits, ReaderFactory factory, std::shared_ptr<jobs::Pool> pool) : pool_(std::move(pool)) {
    if (!pool_ || !factory || !limits.entries || limits.entries > 4096 || limits.thumbnailBytes < 65536 || limits.thumbnailBytes > 256u * 1024 * 1024 || limits.bytes < limits.thumbnailBytes) throw std::invalid_argument("waveform cache limits");
    impl_ = std::make_shared<Impl>(limits, std::move(factory));
}
Cache::~Cache() = default;
std::shared_ptr<Cache> Cache::shared() {
    static std::mutex mutex;
    static std::weak_ptr<Cache> previous;
    std::lock_guard lock(mutex);
    auto cache = previous.lock();
    if (!cache) { cache = std::make_shared<Cache>(); previous = cache; }
    return cache;
}
jobs::Job Cache::request(std::filesystem::path path, jobs::Token token, bool refresh) {
    if (path.empty()) return {};
    auto shared = impl_; std::uint64_t generation;
    { std::lock_guard lock(shared->mutex); if (refresh) { ++shared->generation; shared->entries.clear(); shared->bytes = 0; } generation = shared->generation; }
    return pool_->submit(std::move(token), [shared, path = std::move(path), generation](jobs::Context& context) {
        const auto key = fingerprint(path);
        { std::lock_guard lock(shared->mutex);
          if (shared->generation == generation) for (auto& entry : shared->entries) if (entry.key == key) { entry.used = ++shared->clock; return jobs::Result::success(entry.thumbnail); } }
        auto reader = shared->factory(path); if (!reader) return jobs::Result::failure("No preview decoder for this file");
        auto thumbnail = build(*reader, shared->limits.thumbnailBytes, context);
        if (!thumbnail || context.cancelled()) return jobs::Result{jobs::Status::cancelled, 0, {}, {}};
        if (key != fingerprint(path)) return jobs::Result::failure("Audio file changed during preview loading");
        { std::lock_guard lock(shared->mutex);
          if (generation == shared->generation) {
              for (auto& entry : shared->entries) if (entry.key == key) { entry.used = ++shared->clock; return jobs::Result::success(entry.thumbnail); }
              while (!shared->entries.empty() && (shared->entries.size() >= shared->limits.entries || shared->bytes + thumbnail->bytes() > shared->limits.bytes)) {
                  auto oldest = std::min_element(shared->entries.begin(), shared->entries.end(), [](const auto& a, const auto& b) { return a.used < b.used; });
                  shared->bytes -= oldest->thumbnail->bytes(); shared->entries.erase(oldest);
              }
              shared->entries.push_back({key, thumbnail, ++shared->clock}); shared->bytes += thumbnail->bytes();
          } }
        return jobs::Result::success(std::move(thumbnail));
    }, jobs::Priority::background);
}
void Cache::clear() { std::lock_guard lock(impl_->mutex); ++impl_->generation; impl_->entries.clear(); impl_->bytes = 0; }
std::size_t Cache::residentBytes() const { std::lock_guard lock(impl_->mutex); return impl_->bytes; }
}
