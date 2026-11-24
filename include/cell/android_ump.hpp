// Original Cell Framework implementation; see LICENSE.
#pragma once
#include <cell/ump_device_queue.hpp>

#if defined(__ANDROID__)
#include <amidi/AMidi.h>

namespace cell::midi {

// Control thread constructs and destroys after stopping all I/O. Keep the Java
// MidiDevice open for the lifetime of this connection. Minus one omits a port.
class AndroidUmpPort {
public:
    AndroidUmpPort(JNIEnv*, jobject midiDevice, int sourcePort, int destinationPort);
    ~AndroidUmpPort();
    AndroidUmpPort(const AndroidUmpPort&) = delete;
    AndroidUmpPort& operator=(const AndroidUmpPort&) = delete;

    enum class Poll { empty, data, flushed, rejected, disconnected };
    // Call on the device worker. Poll is nonblocking, one OS packet per call.
    Poll poll(UmpDeviceQueue&) noexcept;
    // Send on the device worker. Timestamp is System.nanoTime() domain, or 0 for immediate.
    bool send(MidiEvent, std::int64_t timestampNs = 0) noexcept;

private:
    void close() noexcept;
    AMidiDevice* device_{};
    AMidiOutputPort* input_{};
    AMidiInputPort* output_{};
};
} // namespace cell::midi
#endif
