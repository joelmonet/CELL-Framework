// Original Cell Framework implementation; see LICENSE.
#pragma once
#include <ARA_API/ARAInterface.h>
#include <atomic>
#include <cstdint>
#include <mutex>
#include <span>
#include <vector>

namespace cell::ara {
struct SourceInfo {
    double sampleRate = 0;
    unsigned numChannels = 0;
    int64_t lengthInSamples = 0;
    bool operator==(const SourceInfo&) const = default;
};
// Capture once when preparing a provider/job. Reopening a source, even with the
// same channel layout, invalidates every earlier snapshot.
struct SourceSnapshot {
    SourceInfo info;
    uint64_t revision = 0;
};
// Direct ARA C ABI; independent of GUI, plugin wrappers and format readers.
// open/close are called only in permitted host model/source callbacks. A close
// revokes queued work and joins the current bounded host read before destroying
// its reader. All read operations are worker-only; playback uses prepared PCM.
class SourceReader final {
  public:
    static constexpr int readQuantum = 8192;
    SourceReader() = default;
    ~SourceReader();
    SourceReader(const SourceReader &) = delete;
    SourceReader &operator=(const SourceReader &) = delete;
    bool open(ARA::ARAAudioAccessControllerHostRef,
              const ARA::ARAAudioAccessControllerInterface *,
              ARA::ARAAudioSourceHostRef, SourceInfo);
    void close() noexcept;
    bool isOpen() const noexcept;
    SourceInfo metadata() const noexcept;
    SourceSnapshot snapshot() const noexcept;
    // The explicit pointer span and source snapshot prevent old queued work
    // from borrowing a replacement source or indexing a changed channel layout.
    // Failure clears only the destination span supplied by this caller.
    bool read(std::span<float *const>, const SourceSnapshot&, int64_t first, int count) noexcept;
  private:
    mutable std::mutex mutex;
    std::atomic<uint64_t> generation{0};
    ARA::ARAAudioAccessControllerHostRef controller = nullptr;
    const ARA::ARAAudioAccessControllerInterface *api = nullptr;
    ARA::ARAAudioReaderHostRef reader = nullptr;
    SourceInfo info;
    std::vector<void *> pointers;
};
} // namespace cell::ara
