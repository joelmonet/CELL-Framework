// Original Cell Framework implementation; see LICENSE.
#include <cell/android_ump.hpp>

namespace cell::midi {
AndroidUmpPort::AndroidUmpPort(JNIEnv* env, jobject midiDevice, int sourcePort, int destinationPort) {
    if (!env || !midiDevice || sourcePort < -1 || destinationPort < -1 || (sourcePort < 0 && destinationPort < 0))
        throw std::invalid_argument("Select an Android UMP device and at least one port");
    if (AMidiDevice_fromJava(env, midiDevice, &device_) != AMEDIA_OK)
        throw std::runtime_error("Could not retain Android MIDI device");
    if (AMidiDevice_getDefaultProtocol(device_) < AMIDI_DEVICE_PROTOCOL_UMP_USE_MIDI_CI ||
        (sourcePort >= 0 && AMidiOutputPort_open(device_, sourcePort, &input_) != AMEDIA_OK) ||
        (destinationPort >= 0 && AMidiInputPort_open(device_, destinationPort, &output_) != AMEDIA_OK)) {
        close();
        throw std::runtime_error("Could not open Android UMP ports");
    }
}
AndroidUmpPort::~AndroidUmpPort() { close(); }

void AndroidUmpPort::close() noexcept {
    if (input_) AMidiOutputPort_close(input_);
    if (output_) AMidiInputPort_close(output_);
    if (device_) (void)AMidiDevice_release(device_);
}

AndroidUmpPort::Poll AndroidUmpPort::poll(UmpDeviceQueue& queue) noexcept {
    if (!input_) return Poll::empty;
    std::array<std::uint8_t, 1024> bytes;
    std::size_t size{};
    std::int32_t opcode{};
    std::int64_t timestamp{};
    const auto result = AMidiOutputPort_receive(input_, &opcode, bytes.data(), bytes.size(), &size, &timestamp);
    if (!result) return Poll::empty;
    if (result < 0) {
        queue.flush();
        return Poll::disconnected;
    }
    if (opcode == AMIDI_OPCODE_FLUSH) {
        queue.flush();
        return Poll::flushed;
    }
    return opcode == AMIDI_OPCODE_DATA && size <= bytes.size() && queue.push(std::span(bytes).first(size), timestamp)
        ? Poll::data : Poll::rejected;
}

bool AndroidUmpPort::send(MidiEvent event, std::int64_t timestampNs) noexcept {
    if (!output_ || timestampNs < 0) return false;
    std::array<std::uint8_t, 16> bytes;
    const auto length = UmpDeviceQueue::encode(event, bytes);
    return length && AMidiInputPort_sendWithTimestamp(output_, bytes.data(), length, timestampNs) ==
        static_cast<ssize_t>(length);
}
} // namespace cell::midi
