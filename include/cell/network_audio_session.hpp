// Original Cell Framework implementation; see LICENSE.
#pragma once

#include <cell/network_audio_packet.hpp>
#include <chrono>
#include <memory>

namespace cell::network_audio {
using PairingKey = std::array<std::byte, 32>;
PairingKey generatePairingKey();

struct SessionFormat { std::uint32_t sampleRate, channels; };
using SessionClock = std::chrono::steady_clock;
using Datagram = std::vector<std::byte>;

// Optional Cell::NetworkAudio component. All calls belong on the transport thread.
// Provides peer authentication and encrypted, replay-protected PCM.
class AuthenticatedSender {
public:
    static constexpr std::size_t overhead = 45;
    static constexpr std::size_t payloadLimit = maxPacketBytes - overhead;
    AuthenticatedSender(const PairingKey&, SessionFormat);
    ~AuthenticatedSender();
    Datagram poll(SessionClock::time_point);
    void receive(std::span<const std::byte>, SessionClock::time_point);
    bool canSend() const noexcept;
    bool confirmed() const noexcept;
    Datagram pack(std::span<const std::byte> pcm);
private:
    struct State;
    std::unique_ptr<State> state_;
};

struct AuthenticatedPacket {
    // A view into receiver-owned storage, valid until the next receive call.
    std::span<const std::byte> pcm;
    Datagram reply;
};

class AuthenticatedReceiver {
public:
    AuthenticatedReceiver(const PairingKey&, SessionFormat);
    ~AuthenticatedReceiver();
    // peer is a transport-defined stable address identity. A caller can delay
    // admitting a new session until the previous playback queue is retired.
    AuthenticatedPacket receive(std::span<const std::byte>, std::uint64_t peer,
                                SessionClock::time_point, bool allowNewSession = true);
    void retire() noexcept;
private:
    struct State;
    std::unique_ptr<State> state_;
};
} // namespace cell::network_audio
