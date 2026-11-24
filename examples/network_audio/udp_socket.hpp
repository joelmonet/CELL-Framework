#pragma once

#ifdef _WIN32
#include <winsock2.h>
#include <ws2tcpip.h>
#else
#include <arpa/inet.h>
#include <cerrno>
#include <poll.h>
#include <sys/socket.h>
#include <unistd.h>
#endif

#include <cstddef>
#include <optional>
#include <span>
#include <stdexcept>

namespace cell::network_audio::example {

class UdpSocket {
#ifdef _WIN32
    struct Winsock {
        Winsock() {
            WSADATA data{};
            if (WSAStartup(MAKEWORD(2, 2), &data) != 0)
                throw std::runtime_error("Could not initialize Winsock");
        }
        ~Winsock() { WSACleanup(); }
    } winsock_;
    using Handle = SOCKET;
    using AddressSize = int;
    static constexpr Handle invalidHandle = INVALID_SOCKET;
#else
    using Handle = int;
    using AddressSize = socklen_t;
    static constexpr Handle invalidHandle = -1;
#endif

public:
    UdpSocket() {
        socket_ = ::socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
        if (socket_ == invalidHandle) throw std::runtime_error("Could not open UDP socket");
#ifdef _WIN32
        u_long nonblocking = 1;
        if (ioctlsocket(socket_, FIONBIO, &nonblocking) != 0) {
            closesocket(socket_);
            throw std::runtime_error("Could not configure nonblocking UDP");
        }
#endif
    }
    ~UdpSocket() {
#ifdef _WIN32
        closesocket(socket_);
#else
        ::close(socket_);
#endif
    }
    UdpSocket(const UdpSocket&) = delete;
    UdpSocket& operator=(const UdpSocket&) = delete;

    void bind(const sockaddr_in& address) {
        if (::bind(socket_, reinterpret_cast<const sockaddr*>(&address), sizeof(address)) != 0)
            throw std::runtime_error("Could not bind UDP socket");
    }

    void connect(const sockaddr_in& address) {
        if (::connect(socket_, reinterpret_cast<const sockaddr*>(&address), sizeof(address)) != 0)
            throw std::runtime_error("Could not connect UDP peer");
    }

    sockaddr_in localAddress() const {
        sockaddr_in address{};
        AddressSize size = sizeof(address);
        if (getsockname(socket_, reinterpret_cast<sockaddr*>(&address), &size) != 0)
            throw std::runtime_error("Could not read local UDP address");
        return address;
    }

    // Best effort may drop a packet on a transient failure or unavailable peer.
    bool send(std::span<const std::byte> packet, bool bestEffort = false) const {
#ifdef _WIN32
        const auto count = ::send(socket_, reinterpret_cast<const char*>(packet.data()),
                                  static_cast<int>(packet.size()), 0);
#else
        const auto count = ::send(socket_, packet.data(), packet.size(), MSG_DONTWAIT);
#endif
        if (count < 0 && bestEffort && recoverableSendError()) return false;
        if (count < 0 || static_cast<std::size_t>(count) != packet.size())
            throw std::runtime_error("UDP send failed");
        return true;
    }

    std::optional<std::size_t> receive(std::span<std::byte> buffer, sockaddr_in& peer,
                                       int timeoutMilliseconds) const {
#ifdef _WIN32
        WSAPOLLFD event{socket_, POLLRDNORM, 0};
        const auto ready = WSAPoll(&event, 1, timeoutMilliseconds);
#else
        pollfd event{socket_, POLLIN, 0};
        const auto ready = poll(&event, 1, timeoutMilliseconds);
#endif
        if (ready == 0 || (ready < 0 && transientError())) return std::nullopt;
        if (ready < 0) throw std::runtime_error("UDP poll failed");
        if (!(event.revents & event.events)) throw std::runtime_error("UDP socket error");
        AddressSize size = sizeof(peer);
        auto* address = reinterpret_cast<sockaddr*>(&peer);
#ifdef _WIN32
        const auto count = recvfrom(socket_, reinterpret_cast<char*>(buffer.data()),
                                    static_cast<int>(buffer.size()), 0, address, &size);
        if (count < 0 && WSAGetLastError() == WSAEMSGSIZE) return std::nullopt;
#else
        const auto count = recvfrom(socket_, buffer.data(), buffer.size(), MSG_DONTWAIT, address, &size);
#endif
        if (count < 0 && transientError()) return std::nullopt;
        if (count < 0) throw std::runtime_error("UDP receive failed");
        return static_cast<std::size_t>(count);
    }

private:
    static bool recoverableSendError() noexcept {
#ifdef _WIN32
        const auto error = WSAGetLastError();
        return error == WSAEINTR || error == WSAEWOULDBLOCK || error == WSAENOBUFS ||
               error == WSAECONNREFUSED || error == WSAECONNRESET ||
               error == WSAEHOSTUNREACH || error == WSAENETUNREACH;
#else
        const auto error = errno;
        return error == EINTR || error == EAGAIN || error == EWOULDBLOCK || error == ENOBUFS ||
               error == ECONNREFUSED || error == EHOSTUNREACH || error == ENETUNREACH;
#endif
    }

    static bool transientError() noexcept {
#ifdef _WIN32
        const auto error = WSAGetLastError();
        return error == WSAEINTR || error == WSAEWOULDBLOCK;
#else
        return errno == EINTR || errno == EAGAIN || errno == EWOULDBLOCK;
#endif
    }

    Handle socket_ = invalidHandle;
};

} // namespace cell::network_audio::example
