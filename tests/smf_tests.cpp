// Original Cell Framework implementation; see LICENSE.
#include <cell/smf.hpp>
#include <cell/memory_stream.hpp>
#include <iostream>
#include <random>
namespace {
using namespace cell::midi;
void need(bool value, const char* message) { if (!value) throw std::runtime_error(message); }
template<class Action> void rejects(Action action) { bool failed{}; try { action(); } catch (const std::invalid_argument&) { failed = true; } need(failed, "Malformed SMF was accepted"); }
std::vector<std::byte> hex(std::string_view text) {
    std::vector<std::byte> result; int nibble = -1;
    for (char c : text) { const auto n = c >= '0' && c <= '9' ? c - '0' : c >= 'a' && c <= 'f' ? c - 'a' + 10 : -1;
        if (n < 0) continue; if (nibble < 0) nibble = n; else { result.push_back(std::byte(nibble * 16 + n)); nibble = -1; } }
    need(nibble < 0, "Odd fixture hex"); return result;
}
SmfFile read(std::span<const std::byte> bytes) { cell::MemoryInputStream input(bytes); return readSmfFile(input); }
class CountedInput final : public cell::InputStream {
public:
    cell::MemoryInputStream input; unsigned reads{}; bool unknown{}; int maximum = 65536;
    explicit CountedInput(std::span<const std::byte> bytes) : input(bytes) {}
    int read(void* data, int count) override { ++reads; return input.read(data, std::min(count, maximum)); }
    std::int64_t getPosition() const override { return input.getPosition(); }
    std::int64_t getTotalLength() const override { return unknown ? -1 : input.getTotalLength(); }
    bool setPosition(std::int64_t value) override { return input.setPosition(value); }
    bool isExhausted() const override { return input.isExhausted(); }
};
}
int main() { try {
    const auto golden = hex(
        "4d546864 00000006 0001 0002 01e0"
        "4d54726b 00000014 00ff510307a120 8360ff51030f4240 8360ff2f00"
        "4d54726b 0000001d 00ff0303e5a3b0 00923c64 81704050 8170823c2a 81704000 8170ff2f00");
    const auto file = read(golden); const SmfTimeMap time(file);
    need(file.type == 1 && file.division == 480 && file.tracks.size() == 2 && file.tracks[1].size() == 6, "Known SMF header/events changed");
    need(file.tracks[1][0].message.getTextFromTextMetaEvent() == cell::Text::fromUTF8(u8"声") && file.tracks[1][2].tick == 240 && file.tracks[1][2].message.getChannel() == 3 && file.tracks[1][4].message.isNoteOff() && file.tracks[1][3].message.getVelocity() == 42, "Running status or Unicode/release velocity was lost");
    need(time.secondsAtTick(240) == .25 && time.secondsAtTick(480) == .5 && time.secondsAtTick(720) == 1. && time.secondsAtTick(960) == 1.5, "Cross-track tempo integration changed note times");
    for (std::size_t cut = 0; cut < golden.size(); ++cut) rejects([&] { read(std::span(golden.data(), cut)); });
    std::mt19937 random(0x534d46);
    for (unsigned iteration=0;iteration<2000;++iteration) {
        auto mutated=golden;const auto edits=1+random()%4;
        for(unsigned n=0;n<edits;++n)mutated[random()%mutated.size()]^=std::byte(1u<<(random()%8));
        try {const auto parsed=read(mutated);cell::MemoryOutputStream normalized;need(writeSmfFile(parsed,normalized),"Accepted fuzz input could not re-encode");const auto reopened=read(normalized.bytes());need(reopened.tracks.size()==parsed.tracks.size(),"Accepted fuzz input lost a track");}
        catch(const std::invalid_argument&) {} // Malformed generated input must fail cleanly.
    }
    CountedInput fragmented(golden); fragmented.maximum = 1; need(readSmfFile(fragmented).tracks[1].size() == 6, "Short reads changed parsed events");
    cell::MemoryOutputStream encoded; need(writeSmfFile(file, encoded), "Native SMF write failed");
    const auto roundtrip = read(encoded.bytes());
    for (std::size_t track = 0; track < file.tracks.size(); ++track)
        for (std::size_t n = 0; n < file.tracks[track].size(); ++n)
            need(file.tracks[track][n].tick == roundtrip.tracks[track][n].tick && file.tracks[track][n].message == roundtrip.tracks[track][n].message, "Native SMF roundtrip changed payload or ticks");
    const auto smpte = read(hex("4d5468640000000600000001e3284d54726b0000000d00903c648930803c0000ff2f00"));
    need(std::abs(SmfTimeMap(smpte).secondsAtTick(1200) - 1.001) < 1.e-12, "29.97 SMPTE was rounded to30fps");
    auto split = file; split.type = 2; rejects([&] { SmfTimeMap map(split); });
    need(SmfTimeMap(split, 0).secondsAtTick(960) == 1.5, "Independent SMF sequence tempo was lost");
    const auto extra = read(hex("4d546864000000080000000101e000004a554e4b0000000201024d54726b0000000400ff2f00"));
    need(extra.tracks.size() == 1, "Header extension/unknown chunk handling failed");
    SmfFile payload; payload.type = 0;
    std::vector<std::uint8_t> large(1024 * 1024 + 113, 42);
    const std::uint8_t escape[]{0xf7, 0x80, 0xff, 0};
    payload.tracks = {{{0, Message::createSysExMessage(large.data(), large.size())}, {1, Message(escape)}, {2, Message::metaEvent(0x7f, large)}, {3, Message::endOfTrack()}}};
    cell::MemoryOutputStream bytes; need(writeSmfFile(payload, bytes), "Large native payload export failed");
    CountedInput buffered(bytes.bytes()); const auto decoded = readSmfFile(buffered);
    need(buffered.reads < 40 && decoded.tracks[0][0].message == payload.tracks[0][0].message && decoded.tracks[0][1].message == payload.tracks[0][1].message && decoded.tracks[0][2].message == payload.tracks[0][2].message, "Payload changed or input used a read per MIDI byte");
    const auto shortAttack = hex("4d546864000000060000000101e04d54726b7fffffff00ff7fffffff7f");
    CountedInput unbounded(shortAttack); unbounded.unknown = true; rejects([&] { readSmfFile(unbounded); });
    need(unbounded.reads < 4, "Truncated unknown-size payload caused excessive reads");
    SmfFile longFile; longFile.type = 0;
    constexpr std::uint64_t far = (UINT64_C(1) << 40) + 17;
    longFile.tracks = {{{far, Message::noteOn(10, 36, 99)}, {far + 9600, Message::noteOff(10, 36)}}};
    cell::MemoryOutputStream longBytes; need(writeSmfFile(longFile, longBytes), "Long native SMF write failed");
    const auto longRead = read(longBytes.bytes());
    const auto on = std::find_if(longRead.tracks[0].begin(), longRead.tracks[0].end(), [](const auto& e) { return e.message.isNoteOn(); });
    need(on != longRead.tracks[0].end() && on->tick == far, "64-bit ticks wrapped or gaps used invalid VLQs");
    longFile.tracks[0][0].tick = UINT64_MAX; longFile.tracks[0].resize(1);
    cell::MemoryOutputStream untouched; rejects([&] { writeSmfFile(longFile, untouched); }); need(untouched.getDataSize() == 0, "Invalid track wrote a partial header before validation");
    auto bad = golden; bad[12] = std::byte{}; bad[13] = std::byte{}; rejects([&] { read(bad); });
    bad = golden; bad.pop_back(); rejects([&] { read(bad); });
    bool cancelled{}; unsigned checks{};
    try { cell::MemoryInputStream source(bytes.bytes()); readSmfFile(source, [&] { if (++checks == 10) throw std::runtime_error("cancel"); }); }
    catch (const std::runtime_error&) { cancelled = true; }
    need(cancelled && checks == 10, "Payload read ignored work cancellation");
    cancelled = false; checks = 0; cell::MemoryOutputStream cancelledOutput;
    try { writeSmfFile(payload, cancelledOutput, [&] { if (++checks == 11) throw std::runtime_error("cancel"); }); }
    catch (const std::runtime_error&) { cancelled = true; }
    need(cancelled && cancelledOutput.getDataSize() < 300000, "Large MIDI output ignored bounded work cancellation");
    std::cout << "PASS native SMF: golden running-status/Unicode/tempo data,short reads,truncation boundaries,SMPTE29.97,type2 isolation,extensions,large SysEx/meta/escape,buffered I/O,64-bit sparse gaps and cancellation\n";
    return 0;
} catch (const std::exception& error) { std::cerr << error.what() << '\n'; return 1; } }
