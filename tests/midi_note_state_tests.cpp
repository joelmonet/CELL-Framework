// SPDX-License-Identifier: Apache-2.0
#include "allocation_watch.hpp"
#include <cell/midi_note_state.hpp>
#include <iostream>
#include <stdexcept>

namespace {
void check(bool ok, const char *why) {
  if (!ok)
    throw std::runtime_error(why);
}
void channels() {
  cell::midi::NoteState state;
  check(state.empty() && state.size() == 0, "Fresh note state is not empty");
  for (unsigned ch = 0; ch < 16; ++ch) {
    check(state.process(0x90 | ch, 69, 100) == (ch == 0),
          "Overlapping channel changed the sounding-pitch union");
    check(state.contains(ch + 1, 69) && state.pressed(ch + 1, 69),
          "A channel failed to own its key");
  }
  for (unsigned ch = 0; ch < 15; ++ch) {
    check(!state.process(0x80 | ch, 69, 0) && state.contains(69) &&
              state.size() == 1,
          "A released channel cancelled another channel's key");
    check(!state.contains(ch + 1, 69), "Released channel retained ownership");
  }
  check(state.process(0x9f, 69, 0) && state.empty(),
        "Velocity-zero Note On did not release the final owner");
  state.process(0x90, 69, 100);
  check(!state.process(0x90, 69, 127),
        "Same-channel retrigger stacked a phantom owner");
  check(state.process(0x80, 69, 50) && state.empty(),
        "Retrigger needed more than one Note Off");
  for (unsigned ch = 0; ch < 16; ++ch)
    state.process(0x90 | ch, 40 + ch, 100);
  for (unsigned ch = 0; ch < 16; ++ch) {
    check(state.process(0xb0 | ch, 123, 0) && state.size() == 15 - ch,
          "Channel All Notes Off changed the wrong key count");
    for (unsigned other = ch + 1; other < 16; ++other)
      check(state.contains(other + 1, 40 + other),
            "Channel panic cleared another channel");
  }
}
void pedals() {
  cell::midi::NoteState state;
  state.process(0x90, 69, 100);
  check(!state.process(0xb0, 64, 64) && state.sustainDown(1),
        "Damper threshold rejected 64");
  check(!state.process(0x80, 69, 0) && state.contains(1, 69) &&
            !state.pressed(1, 69),
        "Damper failed to latch a released key");
  check(!state.process(0xb1, 64, 0) && state.contains(69),
        "Another channel lifted the damper");
  check(state.process(0xb0, 64, 63) && state.empty(),
        "Damper threshold failed to release at 63");
  state.process(0xb0, 64, 127);
  check(!state.process(0x80, 60, 0) && state.empty(),
        "Stray Note Off invented a sustained note");
  state.process(0x90, 60, 100);
  state.process(0x90, 64, 100);
  state.process(0x80, 60, 0);
  check(state.process(0xb0, 121, 0) && !state.contains(60) &&
            state.pressed(1, 64) && !state.sustainDown(1),
        "Reset All Controllers released a physical key or retained a latch");
  for (unsigned cc : {123u, 124u, 125u, 126u, 127u}) {
    state.clear();
    state.process(0x9f, 127, 100);
    state.process(0xbf, 64, 127);
    check(!state.process(0xbf, cc, 0) && state.contains(16, 127) &&
              !state.pressed(16, 127),
          "Channel mode failed to respect the damper");
    check(state.process(0xbf, 120, 0) && state.empty() && state.sustainDown(16),
          "All Sound Off failed to silence, or reset controller state");
  }
  state.clear();
  for (unsigned ch = 0; ch < 16; ++ch)
    for (unsigned note = 0; note < 128; ++note)
      state.process(0x90 | ch, note, 100);
  check(state.size() == 128, "Full channel/pitch matrix lost notes");
  for (unsigned ch = 0; ch < 16; ++ch) {
    state.process(0xb0 | ch, 64, 127);
    state.process(0xb0 | ch, 123, 0);
  }
  check(state.size() == 128, "Full damper matrix lost a target");
  state.clear();
  check(state.empty() && !state.sustainDown(16),
        "Reset retained a damper/target");
}
void bounded() {
  cell::midi::NoteState state;
  state.process(0x90, 60, 100);
  for (auto e : {cell::MidiEvent{0, {0x90, 61, 100}, 0},
                 cell::MidiEvent{0, {0x90, 61, 100}, 2},
                 cell::MidiEvent{0, {0x90, 128, 100}, 3},
                 cell::MidiEvent{0, {0x90, 61, 128}, 3}})
    check(!state.process(e) && state.size() == 1,
          "Malformed MIDI changed key state");
  check(!state.contains(128) && !state.contains(0, 60) &&
            !state.contains(17, 60) && !state.pressed(17, 60) &&
            !state.sustainDown(0),
        "Invalid query indexed note storage");
  unsigned random = 0x1944abeu;
  cell_test::audio = true;
  cell_test::allocations = cell_test::deletions = 0;
  for (unsigned n = 0; n < 20000; ++n) {
    random = random * 1664525u + 1013904223u;
    const unsigned ch = random & 15, note = (random >> 8) & 127;
    if (n % 19 == 0)
      state.process(0xb0 | ch, 64, (random >> 16) & 127);
    else if (n % 23 == 0)
      state.process(0xb0 | ch, 120 + (random % 8), 0);
    else
      state.process((random & 0x10000 ? 0x90 : 0x80) | ch, note, 100);
    unsigned count = 0;
    for (unsigned key = 0; key < 128; ++key) {
      bool any = false;
      for (unsigned channel = 1; channel <= 16; ++channel)
        any |= state.contains(channel, key);
      check(any == state.contains(key), "Channel/union queries diverged");
      count += any;
    }
    check(count == state.size(),
          "Sounding-pitch count diverged under mixed input");
  }
  state.clear();
  cell_test::audio = false;
  check(!cell_test::allocations && !cell_test::deletions,
        "Key state allocated or reclaimed");
}
} // namespace
int main() {
  try {
    channels();
    pedals();
    bounded();
    std::cout
        << "PASS: 16-channel note ownership, damper, channel modes, reset, "
           "bounds and callback allocation guards\n";
  } catch (const std::exception &e) {
    cell_test::audio = false;
    std::cerr << e.what() << '\n';
    return 1;
  }
}
