#pragma once
#include <chrono>
#include <memory>

namespace cell {
// A client performs one bounded piece of disk/decoding work, then supplies its
// next polling delay. This interface is used only on the service's worker.
class ReadAheadClient {
public:
    virtual ~ReadAheadClient() = default;
    virtual std::chrono::milliseconds readAheadSlice() = 0;
};

// One fair worker can serve many inactive/sparse media sources. Register and
// remove clients off the audio thread. remove() is a lifetime barrier: when it
// returns, no callback for that client is running or can start. A callback must
// not remove itself or destroy its service. Client owners remove before their
// callback-visible members are destroyed; the service outlives its clients.
class ReadAheadService {
public:
    ReadAheadService();
    ~ReadAheadService();
    ReadAheadService(const ReadAheadService&) = delete;
    ReadAheadService& operator=(const ReadAheadService&) = delete;
    void add(ReadAheadClient&);
    void remove(ReadAheadClient&) noexcept;
private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};
}
