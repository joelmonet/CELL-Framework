// SPDX-License-Identifier: Apache-2.0
#pragma once
#include <chrono>
namespace cell::ui {
// Defaults developed against real plugin/editor use. Products may override
// sizing and colors while retaining the interaction and host-key contracts.
struct InteractionDefaults {
 static constexpr double wheel=.018,fineWheel=.0012;
 static constexpr double rotaryPixels=210,fineRotaryPixels=2100;
 static constexpr auto submenuHover=std::chrono::milliseconds(160);
 static constexpr auto tooltipDelay=std::chrono::milliseconds(600);
 static constexpr bool nativeFileBrowsing=true,pointerAnchoredMenus=true;
 static constexpr bool escapeReturnsToHost=true,spaceReturnsToHost=true;
 static constexpr bool centerUnityOnKnobs=true,showInsertionMarker=true;
};
struct EditorDefaults {
 static constexpr double width=1320,height=760;
 static constexpr bool dark=true;
 static constexpr unsigned background=0xff293a40,panel=0xff203138,
  surface=0xff344b52,ink=0xffd8e7e6,accent=0xff91ced4,insertion=0xffeaca57;
};
}
