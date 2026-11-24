// Original Cell Framework implementation; see LICENSE.
#include <cell/smf.hpp>
#include <algorithm>
#include <cmath>
#include <cstring>
#include <limits>
#include <stdexcept>

namespace cell::midi {
namespace {
constexpr std::uint32_t largestDelta = 0x0fffffff;
void require(bool value, const char* message) { if (!value) throw std::invalid_argument(message); }
void work(const SmfWorkCheck& check) { if (check) check(); }
void divisionValid(std::uint16_t division) {
    if (!(division & 0x8000)) { require(division != 0, "Zero SMF quarter-note division"); return; }
    const auto fps = 256 - int(division >> 8);
    require((fps == 24 || fps == 25 || fps == 29 || fps == 30) && (division & 255), "Invalid SMF SMPTE division");
}
unsigned vlqSize(std::uint32_t value) noexcept { unsigned count = 1; while ((value >>= 7) != 0) ++count; return count; }
bool writeVlq(OutputStream& output, std::uint32_t value) {
    std::array<std::uint8_t, 4> bytes{}; std::size_t at = bytes.size();
    bytes[--at] = std::uint8_t(value & 127);
    while ((value >>= 7) != 0) bytes[--at] = std::uint8_t((value & 127) | 128);
    return output.write(bytes.data() + at, bytes.size() - at);
}
bool writePayload(OutputStream& output, const std::uint8_t* bytes, std::size_t count, const SmfWorkCheck& check) {
    while (count) {
        work(check); const auto amount = std::min<std::size_t>(count, 65536);
        if (!output.write(bytes, amount)) return false;
        bytes += amount; count -= amount;
    }
    return true;
}
class Reader {
public:
    Reader(InputStream& input, const SmfWorkCheck& check) : input_(input), check_(check) {
        const auto position = input.getPosition(); require(position >= 0, "Invalid MIDI stream position");
        position_ = std::uint64_t(position);
        const auto total = input.getTotalLength(); if (total >= 0) total_ = std::uint64_t(total);
    }
    std::uint64_t position() const noexcept { return position_; }
    void need(std::uint64_t count, std::uint64_t end = UINT64_MAX) const {
        require(position_ <= end && count <= end - position_, "MIDI event crosses its track boundary");
        if (total_) require(position_ <= *total_ && count <= *total_ - position_, "Truncated MIDI input");
    }
    std::uint8_t byte(std::uint64_t end = UINT64_MAX) {
        need(1, end);
        if (at_ == count_) {
            work(check_); const auto got = input_.read(buffer_.data(), int(buffer_.size()));
            require(got > 0 && std::size_t(got) <= buffer_.size(), "Unexpected end of MIDI input");
            at_ = 0; count_ = std::size_t(got);
        }
        ++position_; return buffer_[at_++];
    }
    std::uint32_t integer(unsigned count) { std::uint32_t value{}; for (unsigned n = 0; n < count; ++n) value = (value << 8) | byte(); return value; }
    bool tag(const char* expected) { for (unsigned n = 0; n < 4; ++n) if (byte() != std::uint8_t(expected[n])) return false; return true; }
    struct Variable { std::uint32_t value{}; std::array<std::uint8_t, 4> bytes{}; unsigned size{}; };
    Variable vlq(std::uint64_t end) {
        Variable result;
        for (; result.size < result.bytes.size();) {
            const auto b = byte(end); result.bytes[result.size++] = b;
            result.value = (result.value << 7) | (b & 127);
            if (!(b & 128)) return result;
        }
        throw std::invalid_argument("SMF variable-length field exceeds four bytes");
    }
    void append(std::vector<std::uint8_t>& bytes, std::size_t count, std::uint64_t end) {
        need(count, end);
        while (count) {
            work(check_);
            if (at_ == count_) { const auto first = byte(end); bytes.push_back(first); --count; continue; }
            const auto amount = std::min(count, count_ - at_);
            bytes.insert(bytes.end(), buffer_.begin() + std::ptrdiff_t(at_), buffer_.begin() + std::ptrdiff_t(at_ + amount));
            position_ += amount; at_ += amount; count -= amount;
        }
    }
    void skip(std::uint64_t size) {
        need(size); work(check_);
        if (size <= count_ - at_) { position_ += size; at_ += std::size_t(size); return; }
        const auto end = position_ + size;
        if (end <= std::uint64_t(INT64_MAX) && input_.setPosition(std::int64_t(end))) {
            at_ = count_ = 0; position_ = end; return;
        }
        while (size--) (void)byte();
    }
private:
    InputStream& input_; const SmfWorkCheck& check_;
    std::array<std::uint8_t, 65536> buffer_{};
    std::size_t at_{}, count_{}; std::uint64_t position_{};
    std::optional<std::uint64_t> total_;
};
std::uint32_t micros(const Message& message) {
    require(message.isTempoMetaEvent(), "Malformed SMF tempo event");
    const auto* data = message.getMetaEventData();
    const auto result = (std::uint32_t(data[0]) << 16) | (std::uint32_t(data[1]) << 8) | data[2];
    require(result != 0, "Zero SMF tempo"); return result;
}
SmfTrack readTrack(Reader& reader, std::uint64_t end, const SmfWorkCheck& check) {
    SmfTrack track; std::uint64_t tick{}; std::uint8_t running{};
    while (reader.position() < end) {
        work(check);
        const auto delta = reader.vlq(end).value;
        require(delta <= UINT64_MAX - tick, "SMF cumulative tick overflow"); tick += delta;
        auto first = reader.byte(end); const bool continuation = first < 128;
        const auto status = continuation ? running : first;
        require(status >= 0x80, "SMF data has no running status");
        std::vector<std::uint8_t> bytes;
        std::array<std::uint8_t, 3> shortBytes{status, 0, 0};
        Message message;
        if (status < 0xf0) {
            running = status;
            const auto count = (status & 0xe0) == 0xc0 ? 2u : 3u;
            shortBytes[1] = continuation ? first : reader.byte(end);
            if (count == 3) shortBytes[2] = reader.byte(end);
            message = Message(std::span(shortBytes.data(), count));
        } else {
            require(!continuation, "Running status cannot name a system message");
            running = 0;
            require(status == 0xff || status == 0xf0 || status == 0xf7, "Invalid SMF system event");
            bytes.push_back(status);
            if (status == 0xff) bytes.push_back(reader.byte(end));
            const auto length = reader.vlq(end);
            if (status == 0xff) bytes.insert(bytes.end(), length.bytes.begin(), length.bytes.begin() + length.size);
            reader.append(bytes, length.value, end);
            message = Message::fromOwnedBytes(std::move(bytes));
        }
        require(message.validForSmf(), "Malformed MIDI event payload");
        if (message.getMetaEventType() == 0x51) (void)micros(message);
        if (message.getMetaEventType() == 0x2f)
            require(message.isEndOfTrackMetaEvent() && reader.position() == end, "Invalid SMF end-of-track placement");
        track.push_back({tick, std::move(message)});
    }
    require(!track.empty() && track.back().message.isEndOfTrackMetaEvent(), "Missing SMF end-of-track event");
    return track;
}
std::uint64_t trackSize(const SmfTrack& track, const SmfWorkCheck& check) {
    std::uint64_t total{}, previous{};
    const auto add = [&](std::uint64_t bytes) { require(bytes <= UINT32_MAX - total, "SMF track exceeds its 32-bit chunk field"); total += bytes; };
    for (std::size_t at = 0; at < track.size(); ++at) {
        work(check); const auto& event = track[at];
        require(event.tick >= previous && event.message.validForSmf(), "Invalid SMF event order or bytes");
        if (event.message.getMetaEventType() == 0x51) (void)micros(event.message);
        if (event.message.getMetaEventType() == 0x2f)
            require(event.message.isEndOfTrackMetaEvent() && at + 1 == track.size(), "SMF end marker must be last");
        auto delta = event.tick - previous;
        const auto spacers = delta ? (delta - 1) / largestDelta : 0;
        require(spacers <= UINT32_MAX / 7, "SMF gap exceeds track chunk capacity");
        add(spacers * 7); delta -= spacers * largestDelta;
        add(vlqSize(std::uint32_t(delta)));
        const auto count = event.message.getRawDataSize();
        const auto status = event.message.getRawData()[0];
        if (status == 0xf0 || status == 0xf7) add(1 + vlqSize(std::uint32_t(count - 1)) + count - 1);
        else add(count);
        previous = event.tick;
    }
    if (track.empty() || !track.back().message.isEndOfTrackMetaEvent()) add(4);
    return total;
}
bool writeTrack(const SmfTrack& track, OutputStream& output, const SmfWorkCheck& check) {
    std::uint64_t previous{};
    const std::uint8_t spacer[]{0xff, 0x7f, 0};
    for (const auto& event : track) {
        work(check); auto delta = event.tick - previous;
        while (delta > largestDelta) {
            work(check);
            if (!writeVlq(output, largestDelta) || !output.write(spacer, sizeof(spacer))) return false;
            delta -= largestDelta;
        }
        if (!writeVlq(output, std::uint32_t(delta))) return false;
        const auto* bytes = event.message.getRawData(); const auto count = event.message.getRawDataSize();
        if (bytes[0] == 0xf0 || bytes[0] == 0xf7) {
            if (!output.write(bytes, 1) || !writeVlq(output, std::uint32_t(count - 1)) || !writePayload(output, bytes + 1, count - 1, check)) return false;
        } else if (!writePayload(output, bytes, count, check)) return false;
        previous = event.tick;
    }
    if (track.empty() || !track.back().message.isEndOfTrackMetaEvent()) {
        const std::uint8_t end[]{0, 0xff, 0x2f, 0};
        if (!output.write(end, sizeof(end))) return false;
    }
    return true;
}
}
SmfFile readSmfFile(InputStream& input, const SmfWorkCheck& check) {
    work(check); Reader reader(input, check);
    require(reader.tag("MThd"), "Missing SMF header");
    const auto header = reader.integer(4); require(header >= 6, "SMF header is too short");
    reader.need(header);
    SmfFile file; file.type = std::uint16_t(reader.integer(2));
    const auto tracks = reader.integer(2); file.division = std::uint16_t(reader.integer(2));
    require(file.type <= 2 && tracks > 0 && (file.type != 0 || tracks == 1), "Invalid SMF format or track count");
    divisionValid(file.division); reader.skip(header - 6);
    for (std::uint32_t at = 0; at < tracks;) {
        work(check); const auto tag = reader.integer(4), length = reader.integer(4); reader.need(length);
        if (tag != 0x4d54726b) { reader.skip(length); continue; }
        file.tracks.push_back(readTrack(reader, reader.position() + length, check)); ++at;
    }
    return file;
}
bool writeSmfFile(const SmfFile& file, OutputStream& output, const SmfWorkCheck& check) {
    work(check); divisionValid(file.division);
    require(file.type <= 2 && !file.tracks.empty() && file.tracks.size() <= UINT16_MAX &&
            (file.type != 0 || file.tracks.size() == 1), "Invalid SMF format or track count");
    std::vector<std::uint32_t> sizes; sizes.reserve(file.tracks.size());
    for (const auto& track : file.tracks) sizes.push_back(std::uint32_t(trackSize(track, check)));
    if (!output.write("MThd", 4) || !output.writeIntBigEndian(6) ||
        !output.writeShortBigEndian(std::bit_cast<std::int16_t>(file.type)) ||
        !output.writeShortBigEndian(std::bit_cast<std::int16_t>(std::uint16_t(file.tracks.size()))) ||
        !output.writeShortBigEndian(std::bit_cast<std::int16_t>(file.division))) return false;
    for (std::size_t track = 0; track < file.tracks.size(); ++track) {
        work(check);
        if (!output.write("MTrk", 4) || !output.writeIntBigEndian(std::bit_cast<std::int32_t>(sizes[track])) ||
            !writeTrack(file.tracks[track], output, check)) return false;
    }
    return true;
}
SmfTimeMap::SmfTimeMap(const SmfFile& file, std::optional<std::size_t> sequence) {
    divisionValid(file.division);
    require(file.type <= 2 && !file.tracks.empty() && (file.type != 2 || sequence.has_value()), "SMF type-2 needs a separate sequence time map");
    if (sequence) require(*sequence < file.tracks.size(), "SMF sequence index is out of range");
    if (file.division & 0x8000) {
        const auto fps = 256 - int(file.division >> 8);
        secondsPerTick_ = (fps == 29 ? 1001.L / 30000.L : 1.L / fps) / (file.division & 255); return;
    }
    ppq_ = file.division;
    struct Tempo { std::uint64_t tick; std::uint32_t micros; };
    std::vector<Tempo> tempos;
    for (std::size_t at = 0; at < file.tracks.size(); ++at) {
        if (sequence && at != *sequence) continue;
        for (const auto& event : file.tracks[at])
            if (event.message.getMetaEventType() == 0x51) tempos.push_back({event.tick, micros(event.message)});
    }
    std::stable_sort(tempos.begin(), tempos.end(), [](const auto& a, const auto& b) { return a.tick < b.tick; });
    segments_.push_back({0, 0, 500000});
    for (const auto& tempo : tempos) {
        auto& prior = segments_.back();
        if (tempo.tick == prior.tick) { prior.micros = tempo.micros; continue; }
        const auto seconds = prior.seconds + (tempo.tick - prior.tick) * (static_cast<long double>(prior.micros) / (1000000.L * ppq_));
        segments_.push_back({tempo.tick, seconds, tempo.micros});
    }
}
double SmfTimeMap::secondsAtTick(std::uint64_t tick) const {
    if (!ppq_) return double(tick * secondsPerTick_);
    const auto found = std::upper_bound(segments_.begin(), segments_.end(), tick,
        [](auto value, const auto& segment) { return value < segment.tick; });
    const auto& segment = *std::prev(found);
    return double(segment.seconds + (tick - segment.tick) * (static_cast<long double>(segment.micros) / (1000000.L * ppq_)));
}
}
