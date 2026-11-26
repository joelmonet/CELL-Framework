#include "gain.hpp"
#include <cell/dsp.hpp>
#include <cell/commands.hpp>
#include <cell/midi.hpp>

namespace cell::example {
class Gain final:public Processor,public ProgramMemory,public web::ActionTarget {
public:
 void attachParameters(const ParameterBank& bank)override{bank_=&bank;}
 void prepare(const ProcessSpec& spec)override{gain_.prepare(spec.sampleRate,.01);reset();}
 void reset()noexcept override{gain_.reset(dbToGain(bank_?bank_->get(gainId):0));}
 void process(AudioBlock<float> audio,const ParameterBank& bank,const Transport&,std::span<const MidiEvent> events)noexcept override{
#if defined(CELL_BANKED_EXAMPLE)
  renderBanked(audio, bank, events, nullptr);
#else
  (void)events;
  render(audio, bank);
#endif
 }
#if defined(CELL_BANKED_EXAMPLE)
 void attachParameterOutput(ParameterOutput* output) override { parameterOutput_ = output; }
 bool routesMidi() const noexcept override { return true; }
 void processMidi(AudioBlock<float> audio, const ParameterBank& bank, const Transport&,
                  std::span<const MidiEvent> events, AudioBlock<float>, midi::Output& output) noexcept override {
  renderBanked(audio, bank, events, &output);
 }
#endif
 ProgramCatalog programs(const ParameterBank& parameters) const override {
  static const auto factory = [] {
   ProgramCatalog catalog;
   for (unsigned bank = 0; bank < slotsPerBank; ++bank) {
    ProgramBank entry{bank, std::string("Bank ") + char('A' + bank), {}};
    for (unsigned program = 0; program < slotsPerBank; ++program)
     entry.programs.push_back({program, std::to_string(int(patchGain({bank, program}))) + " dB Trim"});
    catalog.banks.push_back(std::move(entry));
   }
   return catalog;
  }();
  auto catalog = factory;
  const auto gain = parameters.getAcquire(gainId);
  for (const auto& bank : catalog.banks)
   for (const auto& program : bank.programs)
    if (std::abs(gain - patchGain({bank.number, program.number})) < 1e-8)
     catalog.active = ProgramAddress{bank.number, program.number};
  return catalog;
 }
 bool recallProgram(Engine& engine, ProgramAddress address) override {
  if (address.bank >= slotsPerBank || address.program >= slotsPerBank) return false;
  return engine.editTransaction(std::array{std::pair{gainId, patchGain(address)}});
 }
 bool supportsWebAction(std::string_view key, web::ActionMode mode) const noexcept override {
  return key == factoryRecallAction && mode == web::ActionMode::invoke;
 }
 bool webAction(Engine& engine, std::string_view key, web::ActionPhase phase) override {
  return key == factoryRecallAction && phase == web::ActionPhase::invoke && recallProgram(engine, {5, 0});
 }
private:
 void render(AudioBlock<float> audio, const ParameterBank& bank) noexcept {
  gain_.target(dbToGain(bank.get(gainId)));
  for (unsigned i = 0; i < audio.frames; ++i) {
   const float gain = gain_.next();
   for (unsigned c = 0; c < audio.channelCount; ++c) audio.channel(c)[i] *= gain;
  }
 }
#if defined(CELL_BANKED_EXAMPLE)
 void renderBanked(AudioBlock<float> audio, const ParameterBank& bank,
                   std::span<const MidiEvent> events, midi::Output* output) noexcept {
  unsigned position{};
  for (const auto& event : events) {
   if (event.frame >= audio.frames || event.frame < position) continue;
   if (!event.isSysEx()) {
    if (output) output->push(event);
    continue;
   }
   const auto bytes = event.sysEx;
   if (bytes.size() < 7 || bytes[0] != 0xf0 || bytes[1] != 0x7d || bytes[2] != 0x43 ||
       bytes[3] != 0x47 || bytes.back() != 0xf7 || bytes[5] >= slotsPerBank) continue;

   const auto number = bytes[5];
   if (bytes[4] == 0x01 && bytes.size() == 8 && bytes[6] < slotsPerBank && parameterOutput_) {
    render(audio.slice(position, event.frame - position), bank);
    position = event.frame;
    parameterOutput_->parameterOutput(gainId, patchGain({number, bytes[6]}));
   } else if (bytes[4] == 0x02 && bytes.size() == 7 && output) {
    std::array<std::uint8_t, 13> dump{0xf0, 0x7d, 0x43, 0x47, 0x03, number};
    for (unsigned slot = 0; slot < slotsPerBank; ++slot)
     dump[6 + slot] = static_cast<std::uint8_t>(patchGain({number, slot}) + 60);
    dump.back() = 0xf7;
    output->push(MidiEvent::systemExclusive(event.frame, dump));
   }
  }
  render(audio.slice(position, audio.frames - position), bank);
 }
 ParameterOutput* parameterOutput_{};
#endif
 static constexpr unsigned slotsPerBank = 6;
 static double patchGain(ProgramAddress address) {
  return -60.0 + 2.0 * (address.bank * slotsPerBank + address.program);
 }
 const ParameterBank* bank_{};dsp::SmoothedValue gain_;
};
}
namespace cell::plugin {
const PluginDescriptor& descriptor(){
 static const std::array<Parameter,1> parameters{{{example::gainId,"gain","Gain","dB",-60,12,0,.01,1}}};
 static const PluginDescriptor descriptor{"com.example.cell.gain","Cell Gain Example","Cell SDK Example","0.1.0",2,parameters,true
#if defined(CELL_BANKED_EXAMPLE)
  , 800, 590, false, true, {}, true, true
#endif
 };return descriptor;
}
std::unique_ptr<Processor> createProcessor(){return std::make_unique<example::Gain>();}
}
