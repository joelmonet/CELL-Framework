// Original Cell Framework implementation; see LICENSE.
#include "../examples/network_audio/udp_socket.hpp"
#include <cell/network_audio_session.hpp>
#include <cell/network_audio_sender.hpp>
#include <cell/network_audio_receiver.hpp>
#include <iostream>
#include <thread>

namespace {
using namespace cell::network_audio;
using example::UdpSocket;
using namespace std::chrono_literals;
constexpr SessionFormat stereo{48000, 2};
constexpr std::uint64_t peer = 42;

void check(bool ok, const char* reason) {
    if (!ok) throw std::runtime_error(reason);
}

Datagram pair(AuthenticatedSender& sender, AuthenticatedReceiver& receiver, SessionClock::time_point now) {
    auto hello = sender.poll(now);
    auto challenge = receiver.receive(hello, peer, now);
    check(!hello.empty() && challenge.pcm.empty() && !challenge.reply.empty(), "Pairing challenge failed");
    sender.receive(challenge.reply, now);
    check(sender.canSend() && !sender.confirmed(), "Pairing requires proof of authenticated audio delivery");
    return hello;
}

void protocol() {
    const auto key = generatePairingKey();
    AuthenticatedSender sender(key, stereo), stranger(generatePairingKey(), stereo), wrongRate(key, {44100, 2});
    AuthenticatedReceiver receiver(key, stereo);
    auto now = SessionClock::time_point{};
    check(receiver.receive(stranger.poll(now), peer, now).reply.empty(), "Wrong key received a challenge");
    check(receiver.receive(wrongRate.poll(now), peer, now).reply.empty(), "Incorrect sample rate negotiated");
    const auto hello = pair(sender, receiver, now);
    const std::array<Frame<2>, 2> samples{{{.14f, -.1f}, {.2f, -.3f}}};
    const auto pcm = encodePacket<2>(stereo.sampleRate, 0, samples);
    const auto packet = sender.pack(pcm);
    const auto header = packet.size() - pcm.size() - 16;
    check(packet.size() == pcm.size() + AuthenticatedSender::overhead &&
          !std::ranges::equal(std::span(packet).subspan(header, pcm.size()), pcm),
          "Paired link exposed plaintext PCM");
    auto tampered = packet;
    tampered[35] ^= std::byte{1};
    auto editedSequence = packet;
    editedSequence[21] ^= std::byte{0x40};
    auto oldVersion = packet;
    oldVersion[3] = std::byte{'1'};
    check(receiver.receive(pcm, peer, now).pcm.empty() &&
          receiver.receive(tampered, peer, now).pcm.empty() &&
          receiver.receive(editedSequence, peer, now).pcm.empty() &&
          receiver.receive(oldVersion, peer, now).pcm.empty() &&
          receiver.receive(packet, peer + 1, now).pcm.empty(), "Unpaired, altered or misrouted audio was admitted");
    auto accepted = receiver.receive(packet, peer, now);
    check(std::ranges::equal(accepted.pcm, pcm) && !accepted.reply.empty(), "First audio wasn't accepted and acknowledged");
    sender.receive(accepted.reply, now);
    check(sender.confirmed(), "Sender did not confirm receiver");
    check(receiver.receive(packet, peer, now).pcm.empty(), "Replayed audio was admitted");
    const auto earlier = sender.pack(pcm), later = sender.pack(pcm);
    check(!receiver.receive(later, peer, now).pcm.empty() && !receiver.receive(earlier, peer, now).pcm.empty(),
          "Reordered authenticated audio was lost");
    now += 700ms;
    sender.receive(accepted.reply, now);
    now += 600ms;
    auto newHello = sender.poll(now);
    check(!newHello.empty() && hello != newHello && !sender.canSend(), "Replayed heartbeat prevented re-pairing");
    sender.receive(receiver.receive(newHello, peer, now).reply, now);
    const auto resumed = sender.pack(pcm);
    check(resumed != packet, "Re-pairing reused the previous encrypted packet");
    check(receiver.receive(resumed, peer, now, false).pcm.empty(), "New traffic crossed a live stream boundary");
    receiver.retire();
    check(receiver.receive(packet, peer, now).pcm.empty(), "Retirement accepted stale traffic");
    accepted = receiver.receive(resumed, peer, now);
    sender.receive(accepted.reply, now);
    check(std::ranges::equal(accepted.pcm, pcm) && sender.confirmed(), "Re-pairing did not resume authenticated audio");
}

struct DelayedPacket { SessionClock::time_point deadline; Datagram data; };
struct LinkProfile {
    const char* name;
    std::chrono::milliseconds baseDelay, budget;
    unsigned dropEvery, burstEvery, burstLength;
};

void measure(bool wallClock, const LinkProfile& profile) {
    const auto key = generatePairingKey();
    AuthenticatedSender link(key, stereo);
    AuthenticatedReceiver remote(key, stereo);
    Sender<2, 1024> sender(stereo.sampleRate, 0);
    Receiver<2, 8192, 8192> receiver({0, stereo.sampleRate, profile.budget, 960, 800});
    std::optional<UdpSocket> outgoing, incoming;
    if (wallClock) {
        sockaddr_in local{};
        local.sin_family = AF_INET;
        local.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        incoming.emplace();
        incoming->bind(local);
        outgoing.emplace();
        outgoing->connect(incoming->localAddress());
    }
    const auto anchor = wallClock ? SessionClock::now() : SessionClock::time_point{};
    (void)pair(link, remote, anchor);
    std::uint64_t sequence{}, dropped{};
    std::vector<SessionClock::time_point> captureTimes;
    std::vector<double> latencyMs;
    std::vector<DelayedPacket> delayed;
    captureTimes.reserve(4000 * 49);
    latencyMs.reserve(4000 * 48);
    delayed.reserve(96);
    bool corrected{};
    std::array<Frame<2>, 49> captured{};
    std::array<Frame<2>, 48> played{};
    const auto receive = [&](std::span<const std::byte> packet, SessionClock::time_point now) {
        const auto decoded = remote.receive(packet, peer, now);
        check(!decoded.pcm.empty(), "Authenticated stream dropped a valid datagram");
        if (!decoded.reply.empty()) link.receive(decoded.reply, now);
        (void)receiver.receive(decoded.pcm);
    };

    for (unsigned ms = 0; ms < 4000; ++ms) {
        const auto scheduled = anchor + ms * 1ms;
        if (wallClock) std::this_thread::sleep_until(scheduled);
        const auto now = wallClock ? SessionClock::now() : scheduled;
        if (auto hello = link.poll(now); !hello.empty())
            link.receive(remote.receive(hello, peer, now).reply, now);
        // Add 20 frames per second to the sender's nominal clock (+417 ppm).
        const auto block = std::span(captured).first(ms % 50 ? 48 : 49);
        for (auto& frame : block) {
            captureTimes.push_back(now);
            frame = {static_cast<float>(captureTimes.size()), 1.f};
        }
        check(sender.capture(block), "Probe sender overflowed");
        ++sequence;
        const bool lost = (profile.dropEvery && sequence % profile.dropEvery == 0) ||
            (profile.burstEvery && sequence >= profile.burstEvery && sequence % profile.burstEvery < profile.burstLength);
        if (!lost) {
            auto data = link.pack(sender.takePacket(AuthenticatedSender::payloadLimit));
            check(!data.empty(), "Probe pairing stalled");
            delayed.push_back({now + profile.baseDelay + (ms % 13 == 0 ? 9ms : (ms % 4) * 1ms), std::move(data)});
        } else {
            (void)sender.takePacket(AuthenticatedSender::payloadLimit);
            ++dropped;
        }
        for (auto iterator = delayed.begin(); iterator != delayed.end();) {
            if (iterator->deadline > now) { ++iterator; continue; }
            if (wallClock) check(outgoing->send(iterator->data), "Loopback UDP send failed");
            else receive(iterator->data, now);
            iterator = delayed.erase(iterator);
        }
        if (wallClock) {
            std::array<std::byte, maxPacketBytes + 1> packet{};
            sockaddr_in source{};
            while (const auto bytes = incoming->receive(packet, source, 0))
                receive(std::span(packet).first(*bytes), now);
        }
        while (receiver.service(now - anchor).queued) {}
        if (ms * 1ms < profile.budget + 20ms) continue;
        receiver.render(played);
        const auto playbackTime = wallClock ? SessionClock::now() : now;
        corrected |= receiver.stats().playbackCorrectionPpm > 0;
        for (const auto& frame : played) {
            if (frame[1] != 1 || frame[0] < 1) continue; // Exclude concealment and its interpolation edges.
            const auto index = std::size_t(frame[0]) - 1;
            check(index < captureTimes.size(), "Playout marker was outside the captured timeline");
            latencyMs.push_back(std::chrono::duration<double, std::milli>(playbackTime - captureTimes[index]).count());
        }
    }
    const auto stats = receiver.stats();
    check(corrected && link.confirmed() && (!profile.dropEvery || (dropped && stats.concealedSourceFrames)) &&
          latencyMs.size() > 100000,
          "Probe failed to play through drift and loss");
    std::ranges::sort(latencyMs);
    const auto percentile = [&](std::size_t percentage) { return latencyMs[percentage * (latencyMs.size() - 1) / 100]; };
    if (!wallClock) check(percentile(95) >= profile.baseDelay.count() &&
          percentile(95) < (profile.budget + 40ms).count(), "Simulated playout exceeded the latency budget");
    std::cout << (wallClock ? "UDP loopback software" : "Simulated software") << ", " << profile.name
              << ", delay=" << profile.baseDelay.count() << " ms, budget=" << profile.budget.count()
              << " ms, p50=" << percentile(50) << ", p95=" << percentile(95)
              << ", p99=" << percentile(99) << ", max=" << latencyMs.back()
              << ", dropped=" << dropped << '/' << sequence << ", concealed frames=" << stats.concealedSourceFrames
              << ", underrun frames=" << stats.unfilledOutputFrames << ", correction=" << stats.playbackCorrectionPpm
              << " ppm\n";
}
}

int main(int argc, char** argv) {
    try {
        const bool wallClock = argc == 2 && std::string_view(argv[1]) == "--wall-clock";
        check(argc == 1 || wallClock, "Expected no argument or --wall-clock");
        protocol();
        measure(wallClock, {"no-loss", 0ms, 20ms, 0, 0, 0});
        measure(wallClock, {"periodic-loss", 0ms, 20ms, 19, 0, 0});
        measure(wallClock, {"delayed-bursty-loss", 35ms, 55ms, 31, 250, 8});
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
