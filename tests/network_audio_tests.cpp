#include <cell/network_audio_clock.hpp>
#include <cell/network_audio_packet.hpp>
#include <cell/network_audio_reorder.hpp>
#include <cell/network_audio_receiver.hpp>
#include <cell/network_audio_sender.hpp>

#include <array>

int main() {
    using Frame = cell::network_audio::Frame<2>;
    cell::network_audio::ReceiveBuffer<2, 4> buffer(3);
    constexpr std::array input{Frame{1, -1}, Frame{2, -2}, Frame{3, -3}, Frame{4, -4}};
    std::array<Frame, 3> output{};
    const auto frames = std::span(input);

    if (!buffer.enqueue(frames.first(2)) || buffer.render(output) != 0 ||
        output != std::array<Frame, 3>{}) return 1;
    if (buffer.enqueue(frames) || !buffer.enqueue(frames.last(2))) return 2;
    if (buffer.render(output) != 3 || output != std::array{input[0], input[1], input[2]}) return 3;
    if (buffer.render(output) != 1 || output != std::array{input[3], Frame{}, Frame{}}) return 4;
    if (!buffer.enqueue(frames.first(1)) || buffer.render(output) != 0 ||
        output != std::array<Frame, 3>{}) return 5;
    if (buffer.render({}) != 0 || !buffer.enqueue(frames.subspan(1, 2))) return 6;
    if (buffer.render(output) != 3 || output != std::array{input[0], input[1], input[2]}) return 7;

    using namespace cell::network_audio;
    auto packet = encodePacket<2>(48000, 0x100000001, frames);
    if (packet.size() != 56 || packet[0] != std::byte{0x43} ||
        packet[3] != std::byte{0x31} || packet[16] != std::byte{1} ||
        packet[20] != std::byte{1}) return 8;
    std::array<Frame, 4> decoded{};
    const auto result = decodePacket<2>(packet, 48000, decoded);
    if (!result || result->firstSample != 0x100000001 || result->frames != 4 || decoded != input)
        return 9;
    if (decodePacket<2>(packet, 44100, decoded) || decodePacket<2>(packet, 48000, output) ||
        decodePacket<2>(std::span(packet).first(55), 48000, decoded)) return 10;
    packet.push_back(std::byte{});
    if (decodePacket<2>(packet, 48000, decoded)) return 11;
    packet.pop_back();
    packet[26] = std::byte{0x80};
    packet[27] = std::byte{0x7f};
    if (decodePacket<2>(packet, 48000, decoded)) return 12;
    if (!encodePacket<2>(48000, std::numeric_limits<std::uint64_t>::max(), frames).empty() ||
        !encodePacket<2>(48000, 0, std::array<Frame, 148>{}).empty()) return 13;

    ReorderBuffer<2, 5> reorder(96);
    ReceiveBuffer<2, 3> playback(1);
    std::array<Frame, 4> scratch{};
    if (reorder.insert(98, frames.last(2)) != PacketAdmission::accepted ||
        reorder.insert(98, frames.last(2)) != PacketAdmission::overlap ||
        reorder.insert(97, frames.first(2)) != PacketAdmission::overlap ||
        reorder.insert(101, frames.first(1)) != PacketAdmission::outsideWindow ||
        reorder.pump(playback, scratch, 96).queued != 0) return 14;
    if (reorder.insert(96, frames.first(1)) != PacketAdmission::accepted) return 15;
    const auto firstDrain = reorder.pump(playback, scratch, 98);
    if (firstDrain.queued != 3 || firstDrain.missing != 1 || reorder.nextSample() != 99 ||
        reorder.pump(playback, scratch, 100).queued != 0 ||
        playback.render(output) != 3 || output != std::array{input[0], Frame{}, input[2]}) return 16;
    if (reorder.insert(97, frames.first(1)) != PacketAdmission::late ||
        reorder.insert(100, frames.first(1)) != PacketAdmission::accepted) return 17;
    const auto secondDrain = reorder.pump(playback, scratch, 100);
    if (secondDrain.queued != 2 || secondDrain.missing != 0 || reorder.nextSample() != 101 ||
        playback.render(output) != 2 || output != std::array{input[3], input[0], Frame{}}) return 18;
    const auto end = std::numeric_limits<std::uint64_t>::max();
    ReorderBuffer<2, 5> tail(end - 2);
    if (tail.insert(end - 1, frames.last(2)) != PacketAdmission::invalid ||
        tail.insert(end - 1, frames.last(1)) != PacketAdmission::accepted ||
        tail.insert(end - 2, frames.first(1)) != PacketAdmission::accepted) return 19;
    const auto finalDrain = tail.pump(playback, scratch, end - 2);
    if (finalDrain.queued != 2 || finalDrain.missing || tail.nextSample() != end ||
        tail.pump(playback, scratch, end).queued != 0 ||
        playback.render(output) != 2 || output != std::array{input[0], input[3], Frame{}}) return 20;

    using namespace std::chrono_literals;
    const PlayoutClock clock(96, 48000, 20ms);
    if (clock.concealBefore(-1ns) != 96 || clock.concealBefore(20ms) != 96 ||
        clock.concealBefore(20ms + 20833ns) != 96 ||
        clock.concealBefore(20ms + 20834ns) != 97 ||
        clock.concealBefore(1020ms) != 48096) return 21;
    const PlayoutClock nearLimit(end - 1, 48000, 0ns);
    if (nearLimit.concealBefore(1ms) != end ||
        PlayoutClock(96, std::numeric_limits<std::uint32_t>::max(), 0ns)
            .concealBefore(std::chrono::nanoseconds::max()) != end) return 22;
    ReorderBuffer<2, 5> scheduled(96);
    if (scheduled.insert(97, frames.first(1)) != PacketAdmission::accepted ||
        scheduled.pump(playback, scratch, clock.concealBefore(20ms + 20833ns)).queued != 0)
        return 23;
    const auto expiredGap = scheduled.pump(playback, scratch, clock.concealBefore(20ms + 20834ns));
    if (expiredGap.queued != 2 || expiredGap.missing != 1 ||
        playback.render(output) != 2 || output != std::array{Frame{}, input[0], Frame{}}) return 24;

    for (const int driftPpm : {-300, 300}) {
        ReceiveBuffer<2, 1024> adaptive(256, 800);
        std::array<Frame, 256> initial;
        initial.fill(Frame{0.25f, -0.25f});
        if (!adaptive.enqueue(initial)) return 25;
        std::array<Frame, 65> incoming;
        incoming.fill(initial.front());
        std::array<Frame, 64> block;
        int carry = 0;
        for (int tick = 0; tick < 24000; ++tick) {
            if (adaptive.render(block) != block.size() ||
                !std::ranges::all_of(block, [&](const auto& frame) { return frame == initial.front(); }))
                return 26;
            carry += 64 * driftPpm;
            const auto inputCount = 64 + carry / 1'000'000;
            carry %= 1'000'000;
            if (!adaptive.enqueue(std::span(incoming).first(inputCount))) return 27;
        }
        const auto fill = 1024 - adaptive.writableFrames();
        if (fill < 210 || fill > 302) return 28;
    }
    ReceiveBuffer<2, 4> recovering(3, 800);
    if (!recovering.enqueue(frames) || recovering.render(output) != 3 ||
        recovering.render(output) >= output.size() || recovering.render(output) != 0 ||
        !recovering.enqueue(frames) || recovering.render(output) != 3 ||
        output.front() != input.front()) return 29;

    Receiver<2, 8, 3> receiver({96, 48000, 20ms, 3});
    const auto future = encodePacket<2>(48000, 98, frames.last(2));
    const auto first = encodePacket<2>(48000, 96, frames.first(1));
    if (receiver.receive({}) != PacketAdmission::invalid ||
        receiver.receive(future) != PacketAdmission::accepted ||
        receiver.receive(future) != PacketAdmission::overlap ||
        receiver.service(10ms).queued != 0 ||
        receiver.receive(first) != PacketAdmission::accepted ||
        receiver.service(10ms).queued != 1 || receiver.render(output) != 0) return 30;
    const auto filled = receiver.service(20ms + 41667ns);
    if (filled.queued != 2 || filled.missing != 1 ||
        receiver.service(21ms).queued != 0 ||
        receiver.render(output) != 3 || output != std::array{input[0], Frame{}, input[2]} ||
        receiver.receive(first) != PacketAdmission::late) return 31;

    Sender<2, 4> sender(48000, 100);
    if (!sender.takePacket().empty() || !sender.capture(frames.first(2)) ||
        sender.capture(frames.last(3)) || !sender.capture(frames.last(2))) return 32;
    const auto early = decodePacket<2>(sender.takePacket(), 48000, decoded);
    if (!early || early->firstSample != 100 || early->frames != 2 ||
        decoded[0] != input[0] || decoded[1] != input[1]) return 33;
    const auto afterDrop = decodePacket<2>(sender.takePacket(), 48000, decoded);
    if (!afterDrop || afterDrop->firstSample != 105 || afterDrop->frames != 2 ||
        decoded[0] != input[2] || decoded[1] != input[3] || !sender.takePacket().empty()) return 34;

    Sender<2, 200> split(48000, 0);
    std::array<Frame, 148> large{};
    large.back() = Frame{std::numeric_limits<float>::infinity(), 0.5f};
    if (!split.capture(large) || split.takePacket().size() != maxPacketBytes) return 35;
    const auto last = decodePacket<2>(split.takePacket(), 48000, decoded);
    if (!last || last->firstSample != 147 || last->frames != 1 || decoded[0] != Frame{0, 0.5f}) return 36;

    const auto outside = encodePacket<2>(48000, 200, frames.first(1));
    if (receiver.receive(outside) != PacketAdmission::outsideWindow) return 37;
    const auto stats = receiver.stats();
    if (stats.acceptedPackets != 2 || stats.invalidPackets != 1 || stats.latePackets != 1 ||
        stats.outsideWindowPackets != 1 || stats.overlappingPackets != 1 ||
        stats.concealedSourceFrames != 1 || stats.unfilledOutputFrames != 3) return 38;

    Receiver<2, 3, 3> joining({0, 48000, 20ms, 1, 0, true});
    constexpr std::uint64_t livePosition = 1ULL << 34;
    const auto tooWide = encodePacket<2>(48000, livePosition - 100, frames);
    const auto liveFirst = encodePacket<2>(48000, livePosition, frames.first(1));
    const auto liveLater = encodePacket<2>(48000, livePosition + 2, frames.last(1));
    if (joining.receive({}) != PacketAdmission::invalid ||
        joining.receive(tooWide) != PacketAdmission::outsideWindow ||
        joining.service(1s).queued != 0 ||
        joining.receive(liveFirst) != PacketAdmission::accepted ||
        joining.receive(first) != PacketAdmission::late ||
        joining.receive(liveLater) != PacketAdmission::accepted) return 39;
    const auto joined = joining.service(0ns);
    if (joined.queued != 1 || joined.missing != 0 || joining.queuedPlaybackFrames() != 1 ||
        joining.render(std::span(output).first(1)) != 1 || output[0] != input[0] ||
        joining.queuedPlaybackFrames() != 0) return 40;
    const auto resumed = joining.service(20ms + 41667ns);
    if (resumed.queued != 2 || resumed.missing != 1 ||
        joining.render(std::span(output).first(2)) != 2 ||
        output[0] != Frame{} || output[1] != input[3]) return 41;
}
