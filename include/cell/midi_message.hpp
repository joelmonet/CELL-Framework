// Original Cell Framework implementation; see LICENSE.
#pragma once
#include <cell/text.hpp>
#include <array>
#include <cstdint>
#include <concepts>
#include <memory>
#include <span>
#include <utility>
#include <vector>

namespace cell::midi {
// Common channel events stay inline. Large immutable payloads share ownership
// across edit snapshots; a mutation never changes another snapshot's bytes.
class Message {
public:
    Message() = default;
    Message(const Message&) = default;
    Message& operator=(const Message&) = default;
    Message(Message&& other) noexcept
        : small_(other.small_), large_(std::move(other.large_)),
          size_(std::exchange(other.size_, 0)), time_(std::exchange(other.time_, 0)) {}
    Message& operator=(Message&& other) noexcept {
        if (this != &other) {
            small_ = other.small_; large_ = std::move(other.large_);
            size_ = std::exchange(other.size_, 0); time_ = std::exchange(other.time_, 0);
        }
        return *this;
    }
    Message(const void*, std::size_t, double time = 0);
    explicit Message(std::span<const std::uint8_t>, double time = 0);
    static Message fromOwnedBytes(std::vector<std::uint8_t> bytes, double time = 0) {
        Message value(std::move(bytes)); value.time_ = time; return value;
    }
    template<class T> requires requires(const T& v) {
        v.getRawData(); v.getRawDataSize(); v.getTimeStamp();
    }
    Message(const T& v) : Message(v.getRawData(), payloadSize(v.getRawDataSize()), v.getTimeStamp()) {}

    const std::uint8_t* getRawData() const noexcept;
    std::size_t getRawDataSize() const noexcept { return size_; }
    double getTimeStamp() const noexcept { return time_; }
    void setTimeStamp(double time) noexcept { time_ = time; }
    Message withTimeStamp(double time) const { auto copy = *this; copy.time_ = time; return copy; }
    bool validForSmf() const noexcept;
    bool operator==(const Message&) const noexcept;

    int getChannel() const noexcept;
    void setChannel(int);
    bool isNoteOn(bool zeroVelocityIsNoteOn = false) const noexcept;
    bool isNoteOff(bool zeroVelocityIsNoteOff = true) const noexcept;
    int getNoteNumber() const noexcept;
    std::uint8_t getVelocity() const noexcept;
    float getFloatVelocity() const noexcept { return getVelocity() / 127.f; }
    void setVelocity(float);
    bool isController() const noexcept;
    bool isControllerOfType(int) const noexcept;
    int getControllerNumber() const noexcept;
    int getControllerValue() const noexcept;
    bool isPitchWheel() const noexcept;
    int getPitchWheelValue() const noexcept;
    bool isChannelPressure() const noexcept;
    int getChannelPressureValue() const noexcept;
    bool isAftertouch() const noexcept;
    int getAfterTouchValue() const noexcept;
    bool isProgramChange() const noexcept;
    int getProgramChangeNumber() const noexcept;
    bool isSysEx() const noexcept;
    const std::uint8_t* getSysExData() const noexcept;
    std::size_t getSysExDataSize() const noexcept;
    bool isMetaEvent() const noexcept;
    int getMetaEventType() const noexcept;
    std::size_t getMetaEventLength() const noexcept;
    const std::uint8_t* getMetaEventData() const noexcept;
    bool isTextMetaEvent() const noexcept;
    bool isTrackNameEvent() const noexcept;
    Text getTextFromTextMetaEvent() const;
    bool isTempoMetaEvent() const noexcept;
    double getTempoSecondsPerQuarterNote() const noexcept;
    bool isEndOfTrackMetaEvent() const noexcept;

    static Message noteOn(int channel, int key, int velocity);
    static Message noteOff(int channel, int key, int velocity = 0);
    template<std::floating_point Float> static Message noteOn(int channel, int key, Float velocity) {
        return noteOn(channel, key, normalizedVelocity(double(velocity)));
    }
    template<std::floating_point Float> static Message noteOff(int channel, int key, Float velocity) {
        return noteOff(channel, key, normalizedVelocity(double(velocity)));
    }
    static Message controllerEvent(int channel, int controller, int value);
    static Message pitchWheel(int channel, int value);
    static Message channelPressureChange(int channel, int value);
    static Message aftertouchChange(int channel, int key, int value);
    static Message programChange(int channel, int value);
    static Message createSysExMessage(const void*, std::size_t);
    static Message metaEvent(int type, std::span<const std::uint8_t>);
    static Message textMetaEvent(int type, const Text&);
    static Message tempoMetaEvent(int microseconds);
    static Message endOfTrack();
    static Text getMidiNoteName(int key, bool sharps, bool octave, int middleCOctave);
    static const char* getRhythmInstrumentName(int key) noexcept;

private:
    static int normalizedVelocity(double);
    template<class Count> static std::size_t payloadSize(Count count) {
        if (!std::in_range<std::size_t>(count)) throw std::invalid_argument("Invalid MIDI payload length");
        return std::size_t(count);
    }
    static constexpr std::size_t inlineBytes = 16;
    std::array<std::uint8_t, inlineBytes> small_{};
    std::shared_ptr<const std::vector<std::uint8_t>> large_;
    std::size_t size_{};
    double time_{};
    explicit Message(std::vector<std::uint8_t>);
    bool kind(int status, std::size_t length) const noexcept;
    std::span<const std::uint8_t> metaData() const noexcept;
    void changeByte(std::size_t, std::uint8_t);
};
}
