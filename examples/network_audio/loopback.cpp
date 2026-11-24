#include "udp_socket.hpp"

#include <cell/network_audio_receiver.hpp>
#include <cell/network_audio_sender.hpp>

#include <iostream>

namespace {
using namespace cell::network_audio;
using StereoReceiver = Receiver<2, 8, 4>;
using StereoFrame = Frame<2>;

using cell::network_audio::example::UdpSocket;

bool transfer(const UdpSocket& socket, std::span<const std::byte> packet,
              StereoReceiver& receiver, PacketAdmission expected) {
    if (packet.empty()) return false;
    socket.send(packet);
    std::array<std::byte, maxPacketBytes + 1> input{};
    sockaddr_in peer{};
    const auto size = socket.receive(input, peer, 1000);
    return size && receiver.receive(std::span(input).first(*size)) == expected;
}

bool run() {
    using namespace std::chrono_literals;
    UdpSocket socket;
    sockaddr_in address{};
    address.sin_family = AF_INET;
    address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    socket.bind(address);
    socket.connect(socket.localAddress());

    constexpr std::uint32_t rate = 48000;
    constexpr std::uint64_t firstSample = 960;
    constexpr StereoFrame first{0.25f, -0.25f};
    constexpr std::array later{StereoFrame{0.5f, -0.5f}, StereoFrame{0.75f, -0.75f}};
    StereoReceiver receiver({firstSample, rate, 20ms, 4});
    Sender<2, 2> sender(rate, firstSample);
    if (!sender.capture(std::span(&first, 1))) return false;
    const auto initial = sender.takePacket();
    if (!sender.capture(std::span(&first, 1)) || sender.takePacket().empty() ||
        !sender.capture(later)) return false;
    const auto future = sender.takePacket();

    if (!transfer(socket, future, receiver, PacketAdmission::accepted) ||
        !transfer(socket, future, receiver, PacketAdmission::overlap) ||
        receiver.service(0ns).queued != 0 ||
        !transfer(socket, initial, receiver, PacketAdmission::accepted) ||
        receiver.service(0ns).queued != 1) {
        std::cerr << "UDP transfer or reordering failed.\n";
        return false;
    }

    // Advance a synthetic clock so the expected gap is independent of scheduling.
    const auto drained = receiver.service(20ms + 41667ns);
    std::array<StereoFrame, 4> output{};
    if (drained.queued != 3 || drained.missing != 1 ||
        receiver.render(output) != output.size() ||
        output != std::array{first, StereoFrame{}, later[0], later[1]}) {
        std::cerr << "Playback did not match the expected stereo frames.\n";
        return false;
    }

    const auto stats = receiver.stats();
    if (stats.acceptedPackets != 2 || stats.overlappingPackets != 1 ||
        stats.concealedSourceFrames != 1 || stats.unfilledOutputFrames != 0) {
        std::cerr << "Receiver diagnostics did not match the test stream.\n";
        return false;
    }
    std::cout << "UDP loopback verified: stereo PCM reordered, duplicate rejected, missing frame concealed.\n";
    return true;
}
} // namespace

int main() {
    try {
        return run() ? 0 : 1;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
