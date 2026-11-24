#include "gain.hpp"
#include <cell/dsp.hpp>
#include <cell/commands.hpp>

namespace cell::example {
class Gain final:public Processor,public ProgramMemory,public web::ActionTarget {
public:
 void attachParameters(const ParameterBank& bank)override{bank_=&bank;}
 void prepare(const ProcessSpec& spec)override{gain_.prepare(spec.sampleRate,.01);reset();}
 void reset()noexcept override{gain_.reset(dbToGain(bank_?bank_->get(gainId):0));}
 void process(AudioBlock<float> audio,const ParameterBank& bank,const Transport&,std::span<const MidiEvent>)noexcept override{
  gain_.target(dbToGain(bank.get(gainId)));for(unsigned i=0;i<audio.frames;++i){const float gain=gain_.next();for(unsigned c=0;c<audio.channelCount;++c)audio.channel(c)[i]*=gain;}
 }
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
 static const PluginDescriptor descriptor{"com.example.cell.gain","Cell Gain Example","Cell SDK Example","0.1.0",2,parameters,true};return descriptor;
}
std::unique_ptr<Processor> createProcessor(){return std::make_unique<example::Gain>();}
}
