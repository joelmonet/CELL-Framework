// SPDX-License-Identifier: Apache-2.0
#pragma once
#include <cell/midi.hpp>

namespace cell::midi {
struct ControllerMap {
    unsigned source=128,target=1,minimum=0,maximum=127;
    bool inverted{},keepSource{};
    bool operator==(const ControllerMap&)const=default;
};
struct ToolsConfig {
    bool bypass{},notes=true,controllers=true,pitchBend=true,pressure=true,programs=true;
    unsigned inputChannel{}; // 0 = all; 1..16 = a single input channel
    unsigned outputChannel{}; // 0 = channel table; 1..16 = fixed destination
    std::array<unsigned,16> channels{0,1,2,3,4,5,6,7,8,9,10,11,12,13,14,15};
    int transpose{};unsigned keyLow{},keyHigh=127,velocityLow=1,velocityHigh=127;
    double velocityScale=1,velocityCurve=1;unsigned outputVelocityLow=1,outputVelocityHigh=127;
    unsigned root{};std::uint16_t scale=0xfff;
    std::array<int,8> chord{};unsigned chordVoices=1;
    std::array<ControllerMap,4> cc;
    bool operator==(const ToolsConfig&)const=default;
};
struct ToolsStats {std::uint64_t input{},output{},rejected{},overflows{};unsigned held{},sounding{};};
// Fixed ownership ledger: a note-off follows the mapping used by its note-on,
// even after live changes to transpose, filtering, scale, chords or channels.
// Colliding destinations share one sounding note until their last owner ends.
class Tools {
public:
    static constexpr std::size_t voices=2048;
    Tools()noexcept;
    bool configure(const ToolsConfig&)noexcept;
    void reset()noexcept;
    void requestPanic()noexcept{panic_=true;}
    bool process(std::uint32_t frames,std::span<const MidiEvent>,Output&,const Transport& = {})noexcept;
    ToolsStats stats()const noexcept{return stats_;}
private:
    static constexpr std::uint16_t none=0xffff;
    struct Voice {std::uint16_t next=none,key{};std::uint8_t count{};std::array<std::uint16_t,8> destinations{};};
    void clear()noexcept;
    bool panic(Output&,std::uint32_t)noexcept;
    bool send(Output&,std::uint32_t,unsigned,unsigned,unsigned,unsigned=3)noexcept;
    bool noteOn(const MidiEvent&,Output&)noexcept;
    bool noteOff(unsigned key,unsigned velocity,std::uint32_t,Output&)noexcept;
    bool message(const MidiEvent&,Output&)noexcept;
    bool releaseChannel(unsigned,std::uint32_t,Output&)noexcept;
    unsigned destination(unsigned)const noexcept;
    std::uint16_t destinations(unsigned)const noexcept;
    int pitch(int)const noexcept;
    ToolsConfig config_{};ToolsStats stats_{};
    std::array<Voice,voices> voices_{};std::array<std::uint16_t,2048> heads_{},tails_{},owners_{};
    std::array<std::array<std::uint16_t,16>,16> channelOwners_{};
    std::array<std::uint16_t,16> sustain_{};
    std::array<bool,16> sustainDown_{};
    std::uint16_t free_{};bool panic_{},playing_{},clock_{};std::int64_t expected_{};
};
}
