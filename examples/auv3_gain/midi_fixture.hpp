// Original Cell Framework implementation; see LICENSE.
#pragma once

#include <cell/core.hpp>

namespace cell::auv3_example {
inline constexpr std::uint32_t midiFrames = 64;
inline constexpr double midiSampleRate = 44100;
inline constexpr auto transpose = cell::stableId("transpose");
} // namespace cell::auv3_example

#ifdef __OBJC__
#import <AudioToolbox/AudioToolbox.h>

namespace cell::auv3_example {
inline constexpr AudioComponentDescription midiEffect{kAudioUnitType_MIDIProcessor, 0x43744678, 0x43656c6c, 0, 0};
NSString* verifyMidiEffect(AUAudioUnit* unit, bool checkEmptyClockBus = false);
} // namespace cell::auv3_example
#endif
