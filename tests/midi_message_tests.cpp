// Original Cell Framework implementation; see LICENSE.
#include <cell/midi_message.hpp>
#include <cell/memory_stream.hpp>
#include "allocation_watch.hpp"
#include <iostream>
namespace {
void need(bool value, const char* message) { if (!value) throw std::runtime_error(message); }
template<class Action> void rejects(Action action) { bool failed{}; try { action(); } catch (const std::invalid_argument&) { failed = true; } need(failed, "Invalid MIDI data was accepted"); }
}
int main() { try {
    using cell::midi::Message;
    cell_test::allocations = 0; cell_test::audio = true;
    auto on = Message::noteOn(16, 127, 91); auto off = Message::noteOff(16, 127, 43);
    auto bend = Message::pitchWheel(3, 12001); auto cc = Message::controllerEvent(2, 64, 45);
    auto pressure = Message::channelPressureChange(4, 73); auto after = Message::aftertouchChange(5, 60, 31);
    auto program = Message::programChange(6, 127); auto tempo = Message::tempoMetaEvent(500000); auto end = Message::endOfTrack();
    auto copy = on; copy.setChannel(1); copy.setVelocity(.5f); copy.setTimeStamp(123.25);
    cell_test::audio = false;
    need(cell_test::allocations == 0, "Common channel or small meta messages allocated storage");
    need(on.getChannel() == 16 && on.getNoteNumber() == 127 && on.getVelocity() == 91 && copy.getVelocity() == 64 && copy.getChannel() == 1 && on.getTimeStamp() == 0, "Message edit changed its source or MIDI values");
    need(off.isNoteOff() && off.getVelocity() == 43 && Message::noteOn(1, 60, 0).isNoteOff() && !Message::noteOn(1, 60, 0).isNoteOn(), "Note-off/zero-velocity semantics changed");
    need(Message::noteOn(1,60,.5).getVelocity()==64&&Message::noteOff(1,60,.25f).getVelocity()==32,"Normalized velocity was implicitly narrowed to an integer");
    rejects([]{Message::noteOn(1,60,1.5);});
    need(bend.getPitchWheelValue() == 12001 && cc.isControllerOfType(64) && cc.getControllerValue() == 45 && pressure.getChannelPressureValue() == 73 && after.getAfterTouchValue() == 31 && program.getProgramChangeNumber() == 127, "Expression data was narrowed or misclassified");
    need(tempo.isTempoMetaEvent() && tempo.getTempoSecondsPerQuarterNote() == .5 && end.isEndOfTrackMetaEvent() && end.validForSmf(), "Native tempo/end metadata is incorrect");
    const auto title = cell::Text::fromUTF8(u8"Élise 声 🎙");
    const auto name = Message::textMetaEvent(3, title);
    need(name.isTrackNameEvent() && name.getTextFromTextMetaEvent() == title, "UTF-8 MIDI names were corrupted");
    std::vector<std::uint8_t> payload(1024 * 1024 + 113, 42);
    const auto sysex = Message::createSysExMessage(payload.data(), payload.size());
    cell_test::allocations = 0; cell_test::audio = true;
    auto shared = sysex; shared.setTimeStamp(5); const auto shifted = shared.withTimeStamp(9);
    cell_test::audio = false;
    need(cell_test::allocations == 0 && shared.getRawData() == sysex.getRawData() && shifted.getRawData() == sysex.getRawData() && sysex.getTimeStamp() == 0, "Large payload snapshot copied bytes or changed its source");
    need(sysex.isSysEx() && sysex.validForSmf() && sysex.getSysExDataSize() == payload.size() && std::equal(payload.begin(), payload.end(), sysex.getSysExData()), "SysEx ownership changed payload bytes");
    auto movedFrom=sysex;auto moved=std::move(movedFrom);
    need(movedFrom.getRawDataSize()==0&&!movedFrom.validForSmf()&&moved.getRawData()==sysex.getRawData(),"Moved payload retained a length without owning its bytes");
    movedFrom=std::move(moved);need(moved.getRawDataSize()==0&&movedFrom.getRawData()==sysex.getRawData(),"Move assignment broke payload ownership");
    const std::uint8_t bad[]{0x90, 0xff, 60}; need(!Message(bad).validForSmf(), "Invalid channel data accepted");
    const std::uint8_t split[]{0xf7, 0x80, 0xff, 0}; need(Message(split).validForSmf(), "SMF continuation/escape data was filtered");
    rejects([] { Message::noteOn(0, 60, 100); }); rejects([] { Message::pitchWheel(1, 16384); });
    rejects([] { Message::controllerEvent(1, 128, 0); }); rejects([] { Message::tempoMetaEvent(0); });
    struct Foreign { const void* getRawData() const { return "x"; } int getRawDataSize() const { return -1; } double getTimeStamp() const { return 0; } };
    rejects([] { Message value(Foreign{}); });
    const auto cName=Message::getMidiNoteName(60,true,true,4),dbName=Message::getMidiNoteName(61,false,true,4);
    if(cName!="C4"||dbName!="Db4"||!Message::getRhythmInstrumentName(36)||Message::getRhythmInstrumentName(10))std::cerr<<"Naming diagnostic: C="<<cName<<" Db="<<dbName<<" kick="<<(Message::getRhythmInstrumentName(36)!=nullptr)<<" unknown="<<(Message::getRhythmInstrumentName(10)!=nullptr)<<'\n';
    need(cName=="C4"&&dbName=="Db4"&&Message::getRhythmInstrumentName(36)&&!Message::getRhythmInstrumentName(10), "Pitch/drum naming context changed");
    cell::MemoryOutputStream output; need(output.write("abcdef", 6), "Memory output failed");
    need(output.write(output.getData(), 6) && output.getDataSize() == 12 && std::memcmp(output.getData(), "abcdefabcdef", 12) == 0, "Self-append used invalidated memory");
    need(output.setPosition(2) && output.write(output.getData(), 6) && std::memcmp(output.getData(), "ababcdefcdef", 12) == 0, "Overlapping memory write changed bytes");
    cell::MemoryInputStream input(output.bytes()); std::array<char, 12> bytes{};
    need(input.read(bytes.data(), 12) == 12 && input.isExhausted() && input.setPosition(0) && !input.isExhausted(), "Memory input EOF/seek failed");
    need(output.setPosition(INT64_MAX) && !output.write("x", 1) && output.getDataSize() == 12, "Unrepresentable memory output mutated storage");
    std::cout << "PASS native MIDI messages: inline/no-allocation channel/meta events,14-bit bend,pressure,pedal/release velocity,UTF-8,immutable shared SysEx and alias-safe memory streams\n";
    return 0;
} catch (const std::exception& error) { cell_test::audio = false; std::cerr << error.what() << '\n'; return 1; } }
