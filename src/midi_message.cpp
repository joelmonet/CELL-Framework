// Original Cell Framework implementation; see LICENSE.
#include <cell/midi_message.hpp>
#include <algorithm>
#include <cmath>
#include <cstring>
#include <stdexcept>

namespace cell::midi {
namespace {
constexpr std::size_t maximumVlq = 0x0fffffff;
void seven(int value) { if (value < 0 || value > 127) throw std::invalid_argument("Invalid 7-bit MIDI value"); }
std::uint8_t status(int kind, int channel) {
    if (channel < 1 || channel > 16) throw std::invalid_argument("Invalid MIDI channel");
    return std::uint8_t(kind | (channel - 1));
}
Message three(int type, int channel, int a, int b) {
    seven(a); seven(b);
    const std::array<std::uint8_t, 3> bytes{status(type, channel), std::uint8_t(a), std::uint8_t(b)};
    return Message(bytes);
}
Message two(int type, int channel, int a) {
    seven(a);
    const std::array<std::uint8_t, 2> bytes{status(type, channel), std::uint8_t(a)};
    return Message(bytes);
}
}
Message::Message(const void* data, std::size_t size, double time) : size_(size), time_(time) {
    if (size && !data) throw std::invalid_argument("Missing MIDI payload");
    if (size <= inlineBytes) { if (size) std::memcpy(small_.data(), data, size); }
    else {
        auto bytes = std::make_shared<std::vector<std::uint8_t>>(size);
        std::memcpy(bytes->data(), data, size); large_ = std::move(bytes);
    }
}
Message::Message(std::span<const std::uint8_t> bytes, double time)
    : Message(bytes.data(), bytes.size(), time) {}
Message::Message(std::vector<std::uint8_t> bytes) : size_(bytes.size()) {
    if (size_ <= inlineBytes) std::copy(bytes.begin(), bytes.end(), small_.begin());
    else large_ = std::make_shared<const std::vector<std::uint8_t>>(std::move(bytes));
}
const std::uint8_t* Message::getRawData() const noexcept { return large_ ? large_->data() : small_.data(); }
bool Message::operator==(const Message& other) const noexcept {
    return time_ == other.time_ && size_ == other.size_ &&
        (getRawData() == other.getRawData() || std::equal(getRawData(), getRawData() + size_, other.getRawData()));
}
bool Message::kind(int type, std::size_t length) const noexcept {
    return size_ == length && (getRawData()[0] & 0xf0) == type;
}
int Message::getChannel() const noexcept {
    return size_ && getRawData()[0] >= 0x80 && getRawData()[0] < 0xf0 ? (getRawData()[0] & 15) + 1 : 0;
}
void Message::changeByte(std::size_t at, std::uint8_t value) {
    if (large_) { auto bytes = *large_; bytes[at] = value; *this = Message(std::move(bytes)).withTimeStamp(time_); }
    else small_[at] = value;
}
void Message::setChannel(int channel) {
    if (channel < 1 || channel > 16) throw std::invalid_argument("Invalid MIDI channel");
    if (getChannel()) changeByte(0, status(getRawData()[0] & 0xf0, channel));
}
bool Message::isNoteOn(bool zero) const noexcept { return kind(0x90, 3) && (zero || getRawData()[2] != 0); }
bool Message::isNoteOff(bool zero) const noexcept { return kind(0x80, 3) || (zero && kind(0x90, 3) && getRawData()[2] == 0); }
int Message::getNoteNumber() const noexcept { return isNoteOn(true) || isNoteOff() || isAftertouch() ? getRawData()[1] : 0; }
std::uint8_t Message::getVelocity() const noexcept { return isNoteOn(true) || isNoteOff() ? getRawData()[2] : 0; }
void Message::setVelocity(float value) {
    if (!std::isfinite(value)) throw std::invalid_argument("Invalid MIDI velocity");
    if (isNoteOn(true) || isNoteOff()) changeByte(2, std::uint8_t(std::lround(std::clamp(value, 0.f, 1.f) * 127.f)));
}
bool Message::isController() const noexcept { return kind(0xb0, 3); }
bool Message::isControllerOfType(int number) const noexcept { return isController() && getRawData()[1] == number; }
int Message::getControllerNumber() const noexcept { return isController() ? getRawData()[1] : 0; }
int Message::getControllerValue() const noexcept { return isController() ? getRawData()[2] : 0; }
bool Message::isPitchWheel() const noexcept { return kind(0xe0, 3); }
int Message::getPitchWheelValue() const noexcept { return isPitchWheel() ? getRawData()[1] | (getRawData()[2] << 7) : 0; }
bool Message::isChannelPressure() const noexcept { return kind(0xd0, 2); }
int Message::getChannelPressureValue() const noexcept { return isChannelPressure() ? getRawData()[1] : 0; }
bool Message::isAftertouch() const noexcept { return kind(0xa0, 3); }
int Message::getAfterTouchValue() const noexcept { return isAftertouch() ? getRawData()[2] : 0; }
bool Message::isProgramChange() const noexcept { return kind(0xc0, 2); }
int Message::getProgramChangeNumber() const noexcept { return isProgramChange() ? getRawData()[1] : 0; }
bool Message::isSysEx() const noexcept { return size_ && getRawData()[0] == 0xf0; }
const std::uint8_t* Message::getSysExData() const noexcept { return isSysEx() ? getRawData() + 1 : nullptr; }
std::size_t Message::getSysExDataSize() const noexcept {
    return isSysEx() ? size_ - 1 - std::size_t(size_ > 1 && getRawData()[size_ - 1] == 0xf7) : 0;
}
std::span<const std::uint8_t> Message::metaData() const noexcept {
    const auto* bytes = getRawData();
    if (size_ < 3 || bytes[0] != 0xff || bytes[1] > 127) return {};
    std::uint32_t count{};
    for (std::size_t at = 2; at < std::min<std::size_t>(size_, 6); ++at) {
        count = (count << 7) | (bytes[at] & 127);
        if (!(bytes[at] & 128)) return count == size_ - at - 1 ? std::span(bytes + at + 1, count) : std::span<const std::uint8_t>{};
    }
    return {};
}
bool Message::isMetaEvent() const noexcept { return metaData().data() != nullptr; }
int Message::getMetaEventType() const noexcept { return isMetaEvent() ? getRawData()[1] : -1; }
std::size_t Message::getMetaEventLength() const noexcept { return metaData().size(); }
const std::uint8_t* Message::getMetaEventData() const noexcept { return metaData().data(); }
bool Message::isTextMetaEvent() const noexcept { const auto type = getMetaEventType(); return type >= 1 && type <= 15; }
bool Message::isTrackNameEvent() const noexcept { return getMetaEventType() == 3; }
Text Message::getTextFromTextMetaEvent() const {
    const auto data = metaData();
    return isTextMetaEvent() ? Text(std::string_view(reinterpret_cast<const char*>(data.data()), data.size())) : Text{};
}
bool Message::isTempoMetaEvent() const noexcept { return getMetaEventType() == 0x51 && metaData().size() == 3; }
double Message::getTempoSecondsPerQuarterNote() const noexcept {
    const auto bytes = metaData();
    return isTempoMetaEvent() ? double((unsigned(bytes[0]) << 16) | (unsigned(bytes[1]) << 8) | bytes[2]) * .000001 : 0;
}
bool Message::isEndOfTrackMetaEvent() const noexcept { return getMetaEventType() == 0x2f && metaData().empty(); }
bool Message::validForSmf() const noexcept {
    if (!size_) return false;
    const auto* bytes = getRawData();
    const auto type = bytes[0];
    if (type >= 0x80 && type < 0xf0)
        return size_ == (((type & 0xe0) == 0xc0) ? 2u : 3u) &&
            std::all_of(bytes + 1, bytes + size_, [](auto value) { return value < 128; });
    if (type == 0xf0 || type == 0xf7) return size_ - 1 <= maximumVlq;
    return isMetaEvent();
}
Message Message::noteOn(int channel, int key, int velocity) { return three(0x90, channel, key, velocity); }
Message Message::noteOff(int channel, int key, int velocity) { return three(0x80, channel, key, velocity); }
int Message::normalizedVelocity(double value) {
    if (!std::isfinite(value) || value < 0 || value > 1) throw std::invalid_argument("MIDI velocity must be normalized to0..1");
    return int(std::lround(value * 127.));
}
Message Message::controllerEvent(int channel, int number, int value) { return three(0xb0, channel, number, value); }
Message Message::pitchWheel(int channel, int value) {
    if (value < 0 || value > 16383) throw std::invalid_argument("Invalid 14-bit pitch bend");
    return three(0xe0, channel, value & 127, value >> 7);
}
Message Message::channelPressureChange(int channel, int value) { return two(0xd0, channel, value); }
Message Message::aftertouchChange(int channel, int key, int value) { return three(0xa0, channel, key, value); }
Message Message::programChange(int channel, int value) { return two(0xc0, channel, value); }
Message Message::createSysExMessage(const void* data, std::size_t size) {
    if (size > maximumVlq - 1 || (!data && size)) throw std::invalid_argument("Invalid SysEx payload");
    std::vector<std::uint8_t> bytes(size + 2); bytes.front() = 0xf0; bytes.back() = 0xf7;
    if (size) std::memcpy(bytes.data() + 1, data, size);
    if (!std::all_of(bytes.begin() + 1, bytes.end() - 1, [](auto value) { return value < 128; }))
        throw std::invalid_argument("SysEx data must contain 7-bit bytes");
    return Message(std::move(bytes));
}
Message Message::metaEvent(int type, std::span<const std::uint8_t> data) {
    seven(type);
    if (!data.empty() && !data.data()) throw std::invalid_argument("Missing MIDI meta payload");
    if (data.size() > maximumVlq) throw std::length_error("Meta payload exceeds the SMF length field");
    std::array<std::uint8_t, 4> size{}; std::size_t at = size.size();
    auto count = std::uint32_t(data.size()); size[--at] = std::uint8_t(count & 127);
    while ((count >>= 7) != 0) size[--at] = std::uint8_t((count & 127) | 128);
    const auto total = 2 + size.size() - at + data.size();
    if (total <= inlineBytes) {
        std::array<std::uint8_t, inlineBytes> bytes{}; bytes[0] = 0xff; bytes[1] = std::uint8_t(type);
        const auto body = std::copy(size.begin() + std::ptrdiff_t(at), size.end(), bytes.begin() + 2);
        std::copy(data.begin(), data.end(), body);
        return Message(std::span(bytes.data(), total));
    }
    std::vector<std::uint8_t> bytes; bytes.reserve(2 + size.size() - at + data.size());
    bytes.push_back(0xff); bytes.push_back(std::uint8_t(type));
    bytes.insert(bytes.end(), size.begin() + std::ptrdiff_t(at), size.end());
    bytes.insert(bytes.end(), data.begin(), data.end());
    return Message(std::move(bytes));
}
Message Message::textMetaEvent(int type, const Text& text) {
    if (type < 1 || type > 15) throw std::invalid_argument("Invalid MIDI text event type");
    return metaEvent(type, {reinterpret_cast<const std::uint8_t*>(text.toRawUTF8()), text.getNumBytesAsUTF8()});
}
Message Message::tempoMetaEvent(int micros) {
    if (micros < 1 || micros > 0xffffff) throw std::invalid_argument("Tempo exceeds the SMF field");
    const std::array<std::uint8_t, 3> bytes{std::uint8_t(micros >> 16), std::uint8_t(micros >> 8), std::uint8_t(micros)};
    return metaEvent(0x51, bytes);
}
Message Message::endOfTrack() { return metaEvent(0x2f, {}); }
Text Message::getMidiNoteName(int key, bool sharps, bool octave, int middleC) {
    if (key < 0 || key > 127) return {};
    static constexpr const char* sharp[]{"C", "C#", "D", "D#", "E", "F", "F#", "G", "G#", "A", "A#", "B"};
    static constexpr const char* flat[]{"C", "Db", "D", "Eb", "E", "F", "Gb", "G", "Ab", "A", "Bb", "B"};
    // Select the element before choosing the spelling table. This also avoids
    // compiler/sanitizer instrumentation of a conditional array lvalue.
    const auto pitchClass = std::size_t(key % 12);
    Text result(sharps ? sharp[pitchClass] : flat[pitchClass]);
    if (octave) result += Text(key / 12 + middleC - 5);
    return result;
}
const char* Message::getRhythmInstrumentName(int key) noexcept {
    static constexpr const char* names[]{
        "Acoustic kick", "Kick 1", "Side stick", "Acoustic snare", "Hand clap", "Electric snare",
        "Low floor tom", "Closed hi-hat", "High floor tom", "Pedal hi-hat", "Low tom", "Open hi-hat",
        "Low mid tom", "High mid tom", "Crash cymbal 1", "High tom", "Ride cymbal 1", "Chinese cymbal",
        "Ride bell", "Tambourine", "Splash cymbal", "Cowbell", "Crash cymbal 2", "Vibraslap", "Ride cymbal 2",
        "High bongo", "Low bongo", "Mute high conga", "Open high conga", "Low conga", "High timbale", "Low timbale",
        "High agogo", "Low agogo", "Cabasa", "Maracas", "Short whistle", "Long whistle", "Short guiro", "Long guiro",
        "Claves", "High wood block", "Low wood block", "Mute cuica", "Open cuica", "Mute triangle", "Open triangle"};
    return key >= 35 && key <= 81 ? names[key - 35] : nullptr;
}
}
