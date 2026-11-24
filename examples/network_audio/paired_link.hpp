// Original Cell Framework implementation; see LICENSE.
#pragma once
#include "udp_socket.hpp"
#include <cell/files.hpp>
#include <cell/network_audio_session.hpp>
#include <thread>

namespace cell::network_audio::example {
inline PairingKey readPairingKey(const char* path) {
#ifdef CELL_NETWORK_AUDIO_AUTH
    const auto bytes = files::readBounded(std::filesystem::path(path), PairingKey{}.size());
    if (bytes.size() != PairingKey{}.size())
        throw std::invalid_argument("Pairing key must contain exactly 32 random bytes");
    PairingKey key;
    std::ranges::copy(bytes, key.begin());
    return key;
#else
    (void)path;
    throw std::invalid_argument("Build with CELL_SDK_NETWORK_AUTH=ON to use --pair-key");
#endif
}

class SenderLink {
public:
    SenderLink(UdpSocket& socket, const std::optional<PairingKey>& key, unsigned rate) : socket_(socket) {
#ifdef CELL_NETWORK_AUDIO_AUTH
        if (key) session_.emplace(*key, SessionFormat{rate, 2});
#else
        (void)key;
        (void)rate;
#endif
    }
    void poll() {
#ifdef CELL_NETWORK_AUDIO_AUTH
        if (!session_) return;
        if (auto hello = session_->poll(SessionClock::now()); !hello.empty()) socket_.send(hello, true);
        std::array<std::byte, maxPacketBytes + 1> buffer{};
        sockaddr_in peer{};
        for (int i = 0; i < 16; ++i) {
            const auto size = socket_.receive(buffer, peer, 0, true);
            if (!size) break;
            session_->receive(std::span(buffer).first(*size), SessionClock::now());
        }
#endif
    }
    void waitForPairing() {
#ifdef CELL_NETWORK_AUDIO_AUTH
        if (!session_) return;
        const auto deadline = SessionClock::now() + std::chrono::seconds(10);
        while (!session_->canSend()) {
            if (SessionClock::now() >= deadline)
                throw std::runtime_error("No authenticated monitor answered within 10 seconds");
            poll();
            if (!session_->canSend()) std::this_thread::sleep_for(std::chrono::milliseconds(2));
        }
#endif
    }
    std::size_t payloadBudget() const noexcept {
#ifdef CELL_NETWORK_AUDIO_AUTH
        if (session_) return AuthenticatedSender::payloadLimit;
#endif
        return maxPacketBytes;
    }
    bool confirmed() const noexcept {
#ifdef CELL_NETWORK_AUDIO_AUTH
        return session_ && session_->confirmed();
#else
        return false;
#endif
    }
    bool send(std::span<const std::byte> pcm, bool bestEffort) {
#ifdef CELL_NETWORK_AUDIO_AUTH
        if (session_) {
            const auto packet = session_->pack(pcm);
            return !packet.empty() && socket_.send(packet, true);
        }
#endif
        return socket_.send(pcm, bestEffort);
    }
private:
    UdpSocket& socket_;
#ifdef CELL_NETWORK_AUDIO_AUTH
    std::optional<AuthenticatedSender> session_;
#endif
};

class ReceiverLink {
public:
    ReceiverLink(const std::optional<PairingKey>& key, unsigned rate) {
#ifdef CELL_NETWORK_AUDIO_AUTH
        if (key) session_.emplace(*key, SessionFormat{rate, 2});
#else
        (void)key;
        (void)rate;
#endif
    }
    std::span<const std::byte> receive(UdpSocket& socket, std::span<const std::byte> packet,
                                      const sockaddr_in& peer, bool opening) {
#ifdef CELL_NETWORK_AUDIO_AUTH
        if (session_) {
            const auto peerId = (std::uint64_t(peer.sin_addr.s_addr) << 16) | peer.sin_port;
            const auto result = session_->receive(packet, peerId, SessionClock::now(), opening);
            if (!result.reply.empty()) socket.reply(result.reply, peer);
            return result.pcm;
        }
#else
        (void)socket;
        (void)peer;
        (void)opening;
#endif
        return packet;
    }
    bool enabled() const noexcept {
#ifdef CELL_NETWORK_AUDIO_AUTH
        return session_.has_value();
#else
        return false;
#endif
    }
    void retire() noexcept {
#ifdef CELL_NETWORK_AUDIO_AUTH
        if (session_) session_->retire();
#endif
    }
private:
#ifdef CELL_NETWORK_AUDIO_AUTH
    std::optional<AuthenticatedReceiver> session_;
#endif
};
}
