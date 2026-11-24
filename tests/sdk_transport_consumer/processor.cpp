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
const std::array<Parameter, 1> fields{
    {{17, "segment", "Segment fixture", "", 0, 1, 0}}};
class Probe final : public Processor {
public:
  void attachParameters(const ParameterBank &bank) override { parameters = &bank; }
  void prepare(const ProcessSpec &) override { updateTiming(*parameters); }
  std::uint32_t latency() const noexcept override { return latencyFrames; }
  std::uint32_t tail() const noexcept override { return tailFrames; }
  void reset() noexcept override { tailFrames = parameters->get(17) == .75 ? 0 : 128; }
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
