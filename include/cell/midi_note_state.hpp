// SPDX-License-Identifier: Apache-2.0
#pragma once
#include <array>
#include <cell/audio.hpp>
#include <cstdint>

namespace cell::midi {
// Audio-thread-owned MIDI 1.0 key state. Call in sample order. Channels are
// 1..16; pitches are 0..127. Repeated Note On retriggers the same channel/key,
// rather than creating another owner. Different channels retain separate bits.
//
// process() returns whether the union of sounding pitches changed. Key/pedal
// ownership can change while that return value is false. No heap or locks.
class NoteState {
public:
  void clear() noexcept {
    notes_ = {};
    pedals_ = count_ = 0;
  }
  bool empty() const noexcept { return count_ == 0; }
  unsigned size() const noexcept { return count_; }
  bool contains(unsigned note) const noexcept {
    return note < notes_.size() && sounding(notes_[note]);
  }
  bool contains(unsigned channel, unsigned note) const noexcept {
    return note < notes_.size() &&
           ((notes_[note].down | notes_[note].latched) & mask(channel));
  }
  bool pressed(unsigned channel, unsigned note) const noexcept {
    return note < notes_.size() && (notes_[note].down & mask(channel));
  }
  bool sustainDown(unsigned channel) const noexcept {
    return (pedals_ & mask(channel)) != 0;
  }
  bool process(const MidiEvent &event) noexcept {
    if (event.size != 3)
      return false;
    return process(event.data[0], event.data[1], event.data[2]);
  }
  bool process(unsigned status, unsigned first, unsigned second) noexcept {
    if (status < 0x80 || status >= 0xf0 || first > 127 || second > 127)
      return false;
    const auto bit = std::uint16_t(1u << (status & 15));
    const unsigned before = count_, kind = status & 0xf0;
    if (kind == 0x80 || kind == 0x90) {
      auto &note = notes_[first];
      const bool was = sounding(note);
      if (kind == 0x90 && second) {
        note.down |= bit;
        note.latched &= std::uint16_t(~bit);
      } else {
        // A stray Note Off cannot invent a sustained key on this channel.
        if ((note.down & bit) && (pedals_ & bit))
          note.latched |= bit;
        note.down &= std::uint16_t(~bit);
        if (!(pedals_ & bit))
          note.latched &= std::uint16_t(~bit);
      }
      updateCount(was, sounding(note));
    } else if (kind == 0xb0) {
      if (first == 64) {
        pedal(bit, second >= 64);
      } else if (first == 121) {
        // Reset All Controllers releases damper-held keys, keeping keys that
        // are physically down. Other controller values are outside this state.
        pedal(bit, false);
      } else if (first == 120 || first >= 123) {
        for (auto &note : notes_) {
          const bool was = sounding(note);
          if (first != 120 && (pedals_ & bit))
            note.latched |= note.down & bit;
          else
            note.latched &= std::uint16_t(~bit);
          note.down &= std::uint16_t(~bit);
          updateCount(was, sounding(note));
        }
      }
    }
    return count_ != before;
  }

private:
  struct Note {
    std::uint16_t down{}, latched{};
  };
  std::array<Note, 128> notes_{};
  std::uint16_t pedals_{}, count_{};
  static bool sounding(const Note &note) noexcept {
    return (note.down | note.latched) != 0;
  }
  static std::uint16_t mask(unsigned channel) noexcept {
    return channel >= 1 && channel <= 16 ? std::uint16_t(1u << (channel - 1))
                                         : std::uint16_t{};
  }
  void updateCount(bool was, bool now) noexcept {
    if (was != now) {
      if (now)
        ++count_;
      else
        --count_;
    }
  }
  void pedal(std::uint16_t bit, bool down) noexcept {
    if (down) {
      pedals_ |= bit;
    } else {
      pedals_ &= std::uint16_t(~bit);
      for (auto &note : notes_) {
        const bool was = sounding(note);
        note.latched &= std::uint16_t(~bit);
        updateCount(was, sounding(note));
      }
    }
  }
};
} // namespace cell::midi
