// Original Cell Framework implementation; see LICENSE.
#include <cell/ara/source_reader.hpp>
#include <algorithm>
#include <cmath>
#include <cstddef>

namespace cell::ara {
namespace {
// A snapshot belongs to one source lifetime, including across reader objects
// with otherwise identical metadata. No allocation or host call is involved.
std::atomic<uint64_t> nextSourceRevision{1};
uint64_t sourceRevision() noexcept {
    return nextSourceRevision.fetch_add(1,std::memory_order_relaxed);
}
}
SourceReader::~SourceReader() { close(); }
bool SourceReader::open(ARA::ARAAudioAccessControllerHostRef ref,
                         const ARA::ARAAudioAccessControllerInterface *interface,
                         ARA::ARAAudioSourceHostRef source, SourceInfo metadata) {
    if (!interface || interface->structSize <
            offsetof(ARA::ARAAudioAccessControllerInterface, destroyAudioReader) +
            sizeof(interface->destroyAudioReader) ||
        !interface->createAudioReaderForSource || !interface->readAudioSamples ||
        !interface->destroyAudioReader || !source || !std::isfinite(metadata.sampleRate) ||
        metadata.sampleRate <= 0 || !metadata.numChannels || metadata.lengthInSamples < 0)
        return false;
    std::vector<void *> next(metadata.numChannels);
    const std::lock_guard<std::mutex> lock(mutex);
    if (reader)
        return false;
    auto created = interface->createAudioReaderForSource(ref, source, ARA::kARAFalse);
    if (!created)
        return false;
    controller = ref;
    api = interface;
    reader = created;
    info = metadata;
    pointers.swap(next);
    generation.store(sourceRevision(), std::memory_order_release);
    return true;
}
void SourceReader::close() noexcept {
    generation.store(sourceRevision(), std::memory_order_release);
    const std::lock_guard<std::mutex> lock(mutex);
    if (reader)
        api->destroyAudioReader(controller, reader);
    reader = nullptr;
    controller = nullptr;
    api = nullptr;
}
bool SourceReader::isOpen() const noexcept {
    const std::lock_guard<std::mutex> lock(mutex);
    return reader != nullptr;
}
SourceInfo SourceReader::metadata() const noexcept {
    const std::lock_guard<std::mutex> lock(mutex);
    return info;
}
SourceSnapshot SourceReader::snapshot() const noexcept {
    const std::lock_guard<std::mutex> lock(mutex);
    return {info,generation.load(std::memory_order_acquire)};
}
bool SourceReader::read(std::span<float *const> dest, const SourceSnapshot& source,
                         int64_t first, int count) noexcept {
    if (count < 0)
        return false;
    const auto clear = [&] {
        for (auto *channel : dest)
            if (channel)
                std::fill_n(channel, count, 0.f);
    };
    if (dest.size() != source.info.numChannels ||
        std::any_of(dest.begin(),dest.end(),[](auto* channel){return channel==nullptr;})) {
        clear();
        return false;
    }
    const auto description=source.info;
    const auto revision=source.revision;
    bool available{};
    {
        const std::lock_guard<std::mutex> lock(mutex);
        available=reader && revision==generation.load(std::memory_order_acquire) && description==info;
    }
    if (!available) { clear(); return false; }
    if (!count)
        return true;
    // Avoid signed overflow for requests far beyond either source boundary.
    if (first >= description.lengthInSamples || first <= -int64_t(count)) {
        clear();
        return revision==generation.load(std::memory_order_acquire);
    }
    const auto begin = std::max<int64_t>(0, first);
    const auto offset = begin - first;
    const auto amount = std::min<int64_t>(int64_t(count) - offset,
                                         description.lengthInSamples - begin);
    for (unsigned c = 0; c < description.numChannels; ++c) {
        std::fill_n(dest[c], size_t(offset), 0.f);
        std::fill_n(dest[c] + offset + amount, size_t(count - offset - amount), 0.f);
    }
    bool success = true;
    for (int64_t at = 0; at < amount;) {
        const auto frames = std::min<int64_t>(readQuantum, amount - at);
        {
            const std::lock_guard<std::mutex> lock(mutex);
            if (!reader || revision != generation.load(std::memory_order_acquire)) {
                success = false;
                break;
            }
            for (unsigned c = 0; c < description.numChannels; ++c)
                pointers[c] = dest[c] + offset + at;
            if (api->readAudioSamples(controller, reader, begin + at, frames,
                                      pointers.data()) != ARA::kARATrue) {
                success = false;
                break;
            }
        }
        at += frames;
    }
    // Revocation or a failed host read cannot publish a half-old/half-new page.
    if (revision != generation.load(std::memory_order_acquire))
        success = false;
    if (!success)
        clear();
    return success;
}
} // namespace cell::ara
