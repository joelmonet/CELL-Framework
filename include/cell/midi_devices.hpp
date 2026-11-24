#pragma once
#include <cell/core.hpp>
#include <cell/midi.hpp>
#include <filesystem>
#include <functional>
#include <map>
#include <optional>

namespace cell::midi {
struct PadProfile {
  std::string id, name;
  unsigned channel = 1;
  std::array<std::uint8_t, 16> notes{};
  std::string format; // empty = every format; otherwise a lower-case extension
  bool operator==(const PadProfile &) const = default;
};
struct PadMapping {
  bool enabled{};
  unsigned channel = 1;
  std::array<std::uint8_t, 16> input{}, output{};
  bool operator==(const PadMapping &) const = default;
};
bool valid(const PadProfile &) noexcept;
bool valid(const PadMapping &) noexcept;
std::string writePadMapping(const PadMapping &);
PadMapping readPadMapping(std::string_view);
struct PadCapture {
  std::uint64_t token{};
  unsigned channel{}, note{};
  std::uint64_t sequence{};
};
struct PadInputView {
  std::uint64_t owner{};
  PadMapping mapping;
  std::vector<PadCapture> captures;
  bool overflow{};
};
// Serialized control producer/consumer and one audio thread. No device I/O or
// allocation on audio. Private on-screen notes bypass this hardware mapping.
class PadInput {
public:
  PadInput();
  std::uint64_t owner() const noexcept { return owner_; }
  bool mapping(const PadMapping &) noexcept;
  const PadMapping &mapping() const noexcept {
    return desired_;
  } // control thread
  bool capture(std::uint64_t token) noexcept;
  bool cancel(std::uint64_t token) noexcept;
  PadInputView poll(); // control thread, consumes captured note-ons
  bool active() const noexcept;
  // Returns false on insufficient scratch; the caller must reset the DSP.
  bool process(std::span<const MidiEvent>, std::span<MidiEvent>,
               unsigned frames, std::size_t &count) noexcept;
  void resetAudio() noexcept; // stopped/audio thread, DSP must also be reset
private:
  std::uint64_t owner_{};
  PadMapping desired_, audio_;
  SpscQueue<PadMapping, 9> mappings_;
  SpscQueue<PadCapture, 65> captures_;
  std::atomic<std::uint64_t> token_{}, overflow_{};
  std::uint64_t audioToken_{}, captureSequence_{};
  std::array<std::uint8_t, 16 * 128> captureHeld_{};
  struct Held {
    std::uint16_t count{};
    std::uint8_t target{};
  };
  std::array<Held, 16 * 128> held_{};
  std::array<std::uint32_t, 16 * 128> references_{};
  unsigned voices_{};
};
// Two endpoints identify consecutive ascending/descending note layouts only.
// An irregular layout explicitly switches to learning all sixteen pads.
class PadLearner {
public:
  enum class Stage { idle, first, last, individual, ready };
  void begin(std::uint64_t token);
  void individual();
  bool feed(const PadCapture &);
  void cancel() noexcept { stage_ = Stage::idle; }
  Stage stage() const noexcept { return stage_; }
  unsigned nextPad() const noexcept {
    return stage_ == Stage::last         ? 16
           : stage_ == Stage::individual ? count_ + 1
                                         : 1;
  }
  std::uint64_t token() const noexcept { return token_; }
  std::string_view message() const noexcept { return message_; }
  PadProfile profile(std::string id, std::string name) const;

private:
  Stage stage_ = Stage::idle;
  std::uint64_t token_{};
  unsigned channel_{}, count_{};
  std::array<std::uint8_t, 16> notes_{};
  std::string message_;
};
struct PadProfiles {
  std::vector<PadProfile> profiles;
  std::string defaultId;
  std::map<std::string, std::string, std::less<>> formatDefaults;
  std::string defaultFor(std::string_view format) const;
};
std::string padFormat(std::string_view); // canonical extension or empty
// Named user presets are separate from kits/projects. Mutations re-read under
// a non-blocking OS file lock; atomic replacement preserves the previous file.
class PadProfileStore {
public:
  explicit PadProfileStore(std::filesystem::path file)
      : file_(std::move(file)) {}
  static std::filesystem::path userFile();
  PadProfiles read() const;
  void save(const PadProfile &, bool replace = false);
  void
  makeDefault(std::string_view id,
              std::string_view format = {}); // empty id clears this default
  const std::filesystem::path &file() const noexcept { return file_; }

private:
  std::filesystem::path file_;
  void mutate(const std::function<void(PadProfiles &)> &);
};
std::string newPadProfileId();
} // namespace cell::midi
