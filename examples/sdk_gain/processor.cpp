#include "gain.hpp"
#include <cell/dsp.hpp>

namespace cell::example {
class Gain final:public Processor {
public:
 void attachParameters(const ParameterBank& bank)override{bank_=&bank;}
 void prepare(const ProcessSpec& spec)override{gain_.prepare(spec.sampleRate,.01);reset();}
 void reset()noexcept override{gain_.reset(dbToGain(bank_?bank_->get(gainId):0));}
 void process(AudioBlock<float> audio,const ParameterBank& bank,const Transport&,std::span<const MidiEvent>)noexcept override{
  gain_.target(dbToGain(bank.get(gainId)));for(unsigned i=0;i<audio.frames;++i){const float gain=gain_.next();for(unsigned c=0;c<audio.channelCount;++c)audio.channel(c)[i]*=gain;}
 }
private:
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
