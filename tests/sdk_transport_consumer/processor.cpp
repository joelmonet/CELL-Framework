// SPDX-License-Identifier: Apache-2.0
#include <cell/plugin.hpp>
namespace cell::plugin {
namespace {
constexpr std::array layouts{
    MainBusLayout{AudioLayout::stereo, AudioLayout::stereo},
    MainBusLayout{AudioLayout::surround51, AudioLayout::surround51},
    MainBusLayout{AudioLayout::surround712, AudioLayout::surround712},
    MainBusLayout{AudioLayout::surround714, AudioLayout::surround714},
    MainBusLayout{AudioLayout::surround916, AudioLayout::surround916}};
const auto fields = [] {
  std::array<Parameter, 92> result;
  result[0] = {17, "segment", "Segment fixture", "", 0, 1, 0};
  for (unsigned index = 1; index < result.size(); ++index)
    result[index] = {17 + index, "slot" + std::to_string(index), "Slot " + std::to_string(index), "", 0, 1, 0};
  return result;
}();
class Probe final : public Processor, public ParameterPresentationSource, public WritableProgramMemory {
public:
  void attachParameters(const ParameterBank &bank) override { parameters = &bank; }
  void prepare(const ProcessSpec &) override { updateTiming(*parameters); }
  std::uint32_t latency() const noexcept override { return latencyFrames; }
  std::uint32_t tail() const noexcept override { return tailFrames; }
  void reset() noexcept override { tailFrames = parameters->get(17) == .75 ? 0 : 128; }
  void parameterGesture(ParamId id, EditType type) noexcept override {
    if (id == 18) flushSequence = type == EditType::begin ? 1 : (flushSequence == 2 ? 3 : 0);
  }
  void parameterChanged(ParamId id, double value) noexcept override {
    if (id == 18 && flushSequence == 1 && value == 2.0 / 3) flushSequence = 2;
    if (id == 19 && value == 1 && !changed.exchange(true)) ++revision;
  }
  std::uint64_t parameterPresentationRevision() const noexcept override { return revision.load(); }
  std::optional<ParameterPresentation> parameterPresentation(ParamId id) const override {
    if (id < 18 || id >= 17 + fields.size()) return {};
    ParameterPresentation meta{fields[id - 17], id >= 33};
    if (id == 18) {
      meta.field.name = changed.load() ? "Recalled output" : "Delay algorithm";
      meta.field.maximum = changed.load() ? 12 : 3;
      meta.field.defaultValue = 0;
      meta.field.step = 1;
      meta.field.choices = changed.load() ? std::vector<std::string>{} :
          std::vector<std::string>{"Mono", "Stereo", "Symphonic", "Tape"};
    }
    if (id == 20) meta.readOnly = true;
    return meta;
  }
  ProgramCatalog programs(const ParameterBank&) const override {
    ProgramCatalog output;
    for (unsigned bank = 0; bank < 2; ++bank) {
      ProgramBank entry{bank, "Factory " + std::to_string(bank + 1), {}};
      for (unsigned index = 0; index < (bank == 0 ? 128u : 70u); ++index)
        entry.programs.push_back({index, bank == 0 && index == 37 ? slotName :
            "P" + std::to_string(128 * bank + index)});
      output.banks.push_back(std::move(entry));
    }
    return output;
  }
  bool recallProgram(Engine& engine, ProgramAddress address) override {
    if (address.bank >= 2 || address.program >= (address.bank == 0 ? 128u : 70u)) return false;
    const auto value = address.bank == 0 && address.program == 37 && hasStoredPatch ?
        .75 : double(128 * address.bank + address.program) / 197;
    if (!engine.parameters().set(18, value)) return false;
    if (address.bank == 0 && address.program == 37) {
      const std::array<std::uint8_t,5> dump{0xf0,0x7d,37,1,0xf7};
      (void)engine.enqueueMidi(MidiEvent::systemExclusive(19,dump));
    }
    return true;
  }
  ProgramWriteResult store(ProgramAddress address, std::span<const std::byte> bytes) override {
    if (address.bank != 0 || address.program != 37 || bytes.size() != 2 ||
        bytes[0] != std::byte{0x4d} || bytes[1] != std::byte{0x53})
      return {false, "Invalid fixture patch"};
    hasStoredPatch = true;
    slotName = "Imported P37";
    ++catalogVersion;
    return {true, {}};
  }
  ProgramWriteResult rename(ProgramAddress address, std::string_view name) override {
    if (address.bank != 0 || address.program != 37) return {false, "Fixture slot is read-only"};
    slotName = name;
    ++catalogVersion;
    return {true, {}};
  }
  ProgramWriteResult initialize(ProgramAddress address) override {
    if (auto result = rename(address, "P37"); !result.ok) return result;
    hasStoredPatch = false;
    return {true, {}};
  }
  std::uint64_t catalogRevision() const noexcept override { return catalogVersion; }
  void process(AudioBlock<float> audio, const ParameterBank &bank,
               const Transport &t,
               std::span<const MidiEvent>) noexcept override {
    updateTiming(bank);
    for (unsigned n = 0; n < audio.frames; ++n) {
      audio.channel(0)[n] = float(t.tempo);
      audio.channel(1)[n] = t.tempoValid ? 1.f : 0.f;
    }
    if (t.beatPositionValid && audio.frames)
      audio.channel(0)[0] = float(t.beatPosition);
    if (audio.frames > 1 && flushSequence == 3) {
      audio.channel(1)[1] = 3.f;
      flushSequence = 0;
    }
  }
  bool routesMidi() const noexcept override { return true; }
  void processSidechain(AudioBlock<float> audio, const ParameterBank &bank,
                        const Transport &transport, std::span<const MidiEvent> midi,
                        AudioBlock<float> sidechain) noexcept override {
    process(audio, bank, transport, midi);
    if (audio.channelCount == 6 && sidechain.channelCount == 2)
      for (unsigned n = 0; n < audio.frames; ++n)
        audio.channel(5)[n] += sidechain.channel(0)[n] + sidechain.channel(1)[n];
  }
  void processMidi(AudioBlock<float> audio, const ParameterBank &bank,
                   const Transport &transport, std::span<const MidiEvent> input,
                   AudioBlock<float> sidechain, midi::Output &output) noexcept override {
    processSidechain(audio, bank, transport, input, sidechain);
    // Make receipt observable independently of the adapter's output routing.
    for (const auto &event : input) {
      if (event.isSysEx() && event.frame < audio.frames)
        audio.channel(1)[event.frame] = static_cast<float>(event.sysEx.size());
      if (const auto expression = midi::perNoteExpression(event); expression && event.frame < audio.frames)
        audio.channel(1)[event.frame] = 128.f + expression->note;
    }
    midi::forward(input, output, audio.frames);
  }
private:
  void updateTiming(const ParameterBank &bank) noexcept {
    const bool extended = bank.get(17) == .75;
    latencyFrames = extended ? 64 : 32;
    tailFrames = extended ? 256 : 128;
  }
  const ParameterBank *parameters{};
  std::uint32_t latencyFrames{}, tailFrames{};
  unsigned flushSequence{};
  std::string slotName{"P37"};
  std::uint64_t catalogVersion{};
  bool hasStoredPatch{};
  std::atomic<bool> changed{};
  std::atomic<std::uint64_t> revision{};
};
} // namespace
const PluginDescriptor &descriptor() {
  static const PluginDescriptor d{"org.cell.test.transport",
                                  "Cell Transport Test",
                                  "Cell",
                                  "0.1.0",
                                  2,
                                  fields, false, 1000, 620, true, true, layouts, true, true, true};
  return d;
}
std::unique_ptr<Processor> createProcessor() {
  return std::make_unique<Probe>();
}
} // namespace cell::plugin
