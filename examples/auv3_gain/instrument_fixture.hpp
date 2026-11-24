// Original Cell Framework implementation; see LICENSE.
#pragma once

#include <cell/core.hpp>

namespace cell::auv3_example {
inline constexpr auto instrumentLevel = cell::stableId("level");
inline constexpr std::uint32_t instrumentFrames = 64;
inline constexpr double instrumentRate = 48000;
} // namespace cell::auv3_example

#ifdef __OBJC__
#import <AudioToolbox/AudioToolbox.h>

namespace cell::auv3_example {
inline constexpr AudioComponentDescription instrument{kAudioUnitType_MusicDevice, 0x4374496e, 0x43656c6c, 0, 0};
NSString* verifyInstrument(AUAudioUnit* unit);
} // namespace cell::auv3_example
#endif
