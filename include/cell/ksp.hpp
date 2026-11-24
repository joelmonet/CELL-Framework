// SPDX-License-Identifier: Apache-2.0
#pragma once
#include <cell/midi.hpp>
#include <memory>
#include <stdexcept>
#include <string>

namespace cell::ksp {
class CompileError : public std::runtime_error {
public:
  CompileError(unsigned line, std::string message)
      : std::runtime_error("KSP line " + std::to_string(line) + ": " + message),
        line_(line) {}
  unsigned line() const noexcept { return line_; }

private:
  unsigned line_{};
};
struct Limits {
  unsigned sourceBytes = 1024 * 1024, instructions = 65536, variables = 128,
           arrayValues = 8192;
  unsigned voices = 256, callbacks = 64, instructionsPerBlock = 65536,
           instructionsPerCallback = 8192;
};
struct ProgramData;
class Program {
public:
  Program() = default;
  explicit operator bool() const noexcept { return bool(data_); }

private:
  std::shared_ptr<const ProgramData> data_;
  friend Program compile(std::string_view, const Limits &);
  friend class Runtime;
};
// Only the implemented language/command subset is admitted. No unknown command
// is turned into a no-op. Compilation and preparation belong off audio.
Program compile(std::string_view, const Limits & = {});
enum class CommandKind { start, release, tune, volume, pan, midi, panic };
struct VoiceCommand {
  CommandKind kind{};
  std::uint32_t frame{};
  std::int32_t id{}, value{};
  unsigned channel = 1, note{}, velocity{};
  bool relative{}, oneShot{};
  std::int32_t tuneMillicents{}, volumeMillidB{}, panUnits{};
  MidiEvent midi;
  std::uint64_t offsetMicroseconds{};
  std::array<std::uint64_t, 4> groups{}; // up to 256 original group identities
};
class Output {
public:
  virtual ~Output() = default;
  virtual bool push(const VoiceCommand &) noexcept = 0;
};
enum class Fault {
  none,
  instructionBudget,
  callbackCapacity,
  voiceCapacity,
  outputCapacity,
  arrayBounds,
  arithmetic,
  invalidArgument,
  eventIdentity
};
// A bounded event VM, not an audio effect or an NKI decoder. Voice commands
// retain event identity, group masks and documented parameter units so the
// sampler/DSP adapter can implement them without reducing everything to MIDI.
class Runtime {
public:
  Runtime();
  ~Runtime();
  Runtime(const Runtime &) = delete;
  Runtime &operator=(const Runtime &) = delete;
  void prepare(const Program &, double sampleRate, unsigned groups = 1,
               const Limits & = {});
  void reset() noexcept; // caller must also stop/reset its sample voices
  void
  voiceFinished(std::int32_t id) noexcept; // audio thread, natural sample end
  bool process(std::span<const MidiEvent>, unsigned frames, Output &) noexcept;
  Fault fault() const noexcept;
  std::size_t storageBytes() const noexcept;
  std::int32_t variable(std::string_view) const; // stopped/control thread
private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
};
} // namespace cell::ksp
