#include <cell/web_parameter_bridge.hpp>
#include <cell/web_meter.hpp>
#include <iostream>
#include <thread>
using namespace cell;
namespace {
void check(bool v,const char* text){if(!v)throw std::runtime_error(text);}
class Gain final:public Processor{void prepare(const ProcessSpec&)override{}void reset()noexcept override{}void process(AudioBlock<float>,const ParameterBank&,const Transport&,std::span<const MidiEvent>)noexcept override{}};
}
int main(){try{
    const std::array<Parameter,2> fields{{{stableId("gain"),"gain","Gain","dB",-60,12,0,.1},{stableId("private"),"private","Hidden","",0,1,0,1}}};
    PluginDescriptor descriptor{"cell.web.test","Web test","Cell","1",2,fields};Engine engine(descriptor,std::make_unique<Gain>());engine.prepare(48000,64);
    const std::array allowed{stableId("gain")};web::ParameterBridge bridge(engine,allowed);
    const auto before=json::read(bridge.snapshot());check(before["parameters"].array()->size()==1,"bridge exported unlisted parameters");
    check(bridge.apply(R"({"type":"begin","key":"gain"})")&&bridge.activeGestures()==1,"web begin gesture");
    check(bridge.apply(R"({"type":"parameter","key":"gain","value":-9})")&&engine.parameters().get(stableId("gain"))==-9,"web plain value");
    for(auto text:{R"({"type":"parameter","key":"private","value":1})",R"({"type":"parameter","key":"gain","value":100})",R"({"type":"parameter","key":"gain","value":null})",R"({"type":"parameter","key":"gain","value":-8.1234})",R"({"type":"loadFile","key":"gain","value":0})","{bad}"})check(!bridge.apply(text)&&engine.parameters().get(stableId("gain"))==-9,"invalid/unlisted bridge request changed a value");
    check(bridge.apply(R"({"type":"end","key":"gain"})")&&bridge.activeGestures()==0,"web end gesture");
    check(!bridge.apply(R"({"type":"end","key":"gain"})"),"unowned web end");
    check(bridge.apply(R"({"type":"parameter","key":"gain","value":-12.1})"),"fractional step rejected due to floating-point representation");
    check(bridge.apply(R"({"type":"parameter","key":"gain","value":-12})"),"one-shot gesture failed");
    check(bridge.apply(R"({"type":"begin","key":"gain"})"),"begin before cancel");bridge.cancel();check(bridge.activeGestures()==0&&engine.parameters().get(stableId("gain"))==-12,"cancel rolled back accepted value or retained gesture");
    bool duplicate=false;try{const std::array ids{allowed[0],allowed[0]};web::ParameterBridge invalid(engine,ids);}catch(const std::invalid_argument&){duplicate=true;}check(duplicate,"duplicate allow-list accepted");
    double hostValue = -18;
    bool rejectBegin = true;
    std::string calls;
    {
        web::ParameterBridge host({fields,
            [&](ParamId) { return hostValue; },
            [&](ParamId) { if (rejectBegin) return false; calls += 'b'; return true; },
            [&](ParamId, double value, bool active) {
                calls += active ? 'v' : 's';
                hostValue = value;
                return true;
            },
            [&](ParamId) { calls += 'e'; return true; }}, allowed);
        check(!host.apply(R"({"type":"begin","key":"gain"})") && host.activeGestures() == 0,
              "rejected host gesture retained ownership");
        rejectBegin = false;
        check(host.apply(R"({"type":"parameter","key":"gain","value":-8})"), "missing one-shot host write");
        check(host.apply(R"({"type":"begin","key":"gain"})") &&
              host.apply(R"({"type":"begin","key":"gain"})") &&
              host.apply(R"({"type":"parameter","key":"gain","value":-5})"), "missing active host write");
        hostValue = -4;
        const auto hostSnapshot = json::read(host.snapshot());
        const auto* value = hostSnapshot["parameters"].array()->front()["value"].number();
        check(value && *value == -4, "snapshot did not read the host value");
    }
    check(calls == "sbve", "host gesture ownership did not balance on close");
    web::OutputMeter meters;
    AudioBuffer<float> audio(2, 4);
    for (std::uint32_t channel = 0; channel < 2; ++channel)
        std::fill_n(audio.block().channel(channel), 4, channel == 0 ? -0.5f : 0.f);
    auto levels = [&] {
        const auto snapshot = json::read(meters.snapshot());
        check(snapshot["type"].text() == "meters", "meter message type");
        const auto& levels = *snapshot["meters"].array();
        check(levels[0]["key"].text() == "output-peak" && levels[1]["key"].text() == "output-rms", "meter keys");
        return std::array{*levels[0]["value"].number(), *levels[1]["value"].number()};
    };
    meters.publish(audio.block());
    check(levels() == std::array{-60.0, -60.0}, "inactive meter captured audio");
    meters.setEnabled(true);
    meters.publish(audio.block());
    check(levels() == std::array{-6.0, -9.0}, "incorrect multichannel peak or RMS");
    meters.clear();
    check(levels() == std::array{-60.0, -60.0}, "meter did not clear");
    std::cout<<"PASS: web parameter allow-list, bounds, gestures, cancellation and descriptor snapshot\n";
}catch(const std::exception& e){std::cerr<<e.what()<<'\n';return 1;}}
