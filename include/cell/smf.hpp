// Original Cell Framework implementation; see LICENSE.
#pragma once
#include <cell/midi_message.hpp>
#include <cell/streams.hpp>
#include <functional>
#include <optional>

namespace cell::midi {
struct SmfEvent {
    std::uint64_t tick{};
    Message message;
};
using SmfTrack = std::vector<SmfEvent>;
struct SmfFile {
    std::uint16_t type = 1;
    // Positive PPQ or the original signed SMPTE high byte / ticks-per-frame.
    std::uint16_t division = 9600;
    std::vector<SmfTrack> tracks;
};
// Runs outside the audio callback. Cancellation/error callbacks may throw.
// No fixed event, file, or session length cap. Format field widths and available
// address space are checked before narrowing. Input is buffered; output streams
// directly after a sizing/validation pass, without a second whole-track buffer.
using SmfWorkCheck = std::function<void()>;
SmfFile readSmfFile(InputStream&, const SmfWorkCheck& = {});
bool writeSmfFile(const SmfFile&, OutputStream&, const SmfWorkCheck& = {});

class SmfTimeMap {
public:
    explicit SmfTimeMap(const SmfFile&, std::optional<std::size_t> sequence = std::nullopt);
    double secondsAtTick(std::uint64_t) const;
private:
    struct Segment { std::uint64_t tick{}; long double seconds{}; std::uint32_t micros = 500000; };
    std::vector<Segment> segments_;
    std::uint16_t ppq_{};
    long double secondsPerTick_{};
};
}
