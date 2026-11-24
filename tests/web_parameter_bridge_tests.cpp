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
    {
        const std::array watched{stableId("gain"), stableId("private")};
        const std::array readOnly{watched[1]};
        web::ParameterBridge observed(engine, watched, true, {}, readOnly);
        check(engine.parameters().set(watched[1], 1), "could not update observed parameter at source");
        const auto snapshot = json::read(observed.snapshot());
        const auto& snapshotFields = *snapshot["parameters"].array();
        check(std::any_of(snapshotFields.begin(), snapshotFields.end(), [](const auto& field) {
            const auto* value = field["value"].number();
            return field["key"].text() == "private" && value && *value == 1;
        }), "read-only parameter did not reach the web snapshot");
        check(!observed.apply(R"({"type":"begin","key":"private"})") &&
              !observed.apply(R"({"type":"parameter","key":"private","value":0})") &&
              !observed.apply(R"({"type":"momentary-press","key":"private"})") &&
              engine.parameters().get(watched[1]) == 1,
              "read-only source allowed an edit");
    }
    check(before["programs"].isNull() && !bridge.apply(R"({"type":"recall-program","bank":0,"program":0})"),
          "a processor without program memory exposed recall");
    check(bridge.apply(R"({"type":"begin","key":"gain"})")&&bridge.activeGestures()==1,"web begin gesture");
    check(bridge.apply(R"({"type":"parameter","key":"gain","value":-9})")&&engine.parameters().get(stableId("gain"))==-9,"web plain value");
    for(auto text:{R"({"type":"parameter","key":"private","value":1})",R"({"type":"parameter","key":"gain","value":100})",R"({"type":"parameter","key":"gain","value":null})",R"({"type":"parameter","key":"gain","value":-8.1234})",R"({"type":"loadFile","key":"gain","value":0})","{bad}"})check(!bridge.apply(text)&&engine.parameters().get(stableId("gain"))==-9,"invalid/unlisted bridge request changed a value");
    check(bridge.apply(R"({"type":"end","key":"gain"})")&&bridge.activeGestures()==0,"web end gesture");
    check(!bridge.apply(R"({"type":"end","key":"gain"})"),"unowned web end");
    check(bridge.apply(R"({"type":"parameter","key":"gain","value":-12.1})"),"fractional step rejected due to floating-point representation");
    check(bridge.apply(R"({"type":"parameter","key":"gain","value":-12})"),"one-shot gesture failed");
    check(bridge.apply(R"({"type":"begin","key":"gain"})"),"begin before cancel");bridge.cancel();check(bridge.activeGestures()==0&&engine.parameters().get(stableId("gain"))==-12,"cancel rolled back accepted value or retained gesture");
    ParameterEdit queued;
    while (engine.pollUiEdit(queued)) {}
    const auto momentarySnapshot = json::read(bridge.snapshot());
    const auto* momentary = momentarySnapshot["momentary"].boolean();
    check(momentary && *momentary, "engine did not advertise momentaries");
    check(bridge.apply(R"({"type":"momentary-press","key":"gain"})") && engine.parameters().get(allowed[0]) == 12,
          "momentary press did not set maximum");
    check(!bridge.apply(R"({"type":"end","key":"gain"})") &&
          !bridge.apply(R"({"type":"parameter","key":"gain","value":0})"),
          "ordinary messages interfered with the momentary's ownership");
    const std::array toFill{std::pair{allowed[0], 12.0}};
    for (unsigned i = 0; i < 2048 && engine.editValues(toFill); ++i) {}
    check(!engine.editValues(toFill), "test did not reach queue backpressure");
    bridge.cancel();
    check(!bridge.activeGestures() && engine.parameters().get(allowed[0]) == -60,
          "backpressure prevented momentary release");
    ParameterEdit lastValue;
    while (engine.pollUiEdit(queued)) if (queued.type == EditType::value) lastValue = queued;
    check(lastValue.id == allowed[0] && lastValue.value == -60, "host did not get the reserved release value");
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
    {
        using web::ActionBinding;
        using web::ActionMode;
        using web::ActionPhase;
        const std::array allowedActions{ActionBinding{"store", ActionMode::invoke},
            ActionBinding{"tuner", ActionMode::momentary}, ActionBinding{"unavailable", ActionMode::invoke}};
        std::vector<std::pair<std::string, ActionPhase>> calls;
        bool tunerAvailable = true;
        web::ParameterBridge::Endpoint endpoint{fields, [&](ParamId) { return hostValue; },
            [](ParamId) { return true; }, [](ParamId, double, bool) { return true; }, [](ParamId) { return true; }};
        endpoint.supportsAction = [&](std::string_view key, ActionMode mode) {
            return key == "tuner" ? tunerAvailable && mode == ActionMode::momentary :
                (key == "store" || key == "private") && mode == ActionMode::invoke;
        };
        endpoint.action = [&](std::string_view key, ActionPhase phase) {
            calls.emplace_back(key, phase);
            return true;
        };
        web::ParameterBridge actions(std::move(endpoint), {}, allowedActions);
        check(json::read(actions.snapshot())["actions"].array()->size() == 2, "snapshot advertised unavailable commands");
        check(!actions.apply(R"({"type":"action","key":"private","phase":"invoke"})") &&
              !actions.apply(R"({"type":"action","key":"unavailable","phase":"invoke"})") &&
              !actions.apply(R"({"type":"action","key":"tuner","phase":"invoke"})") &&
              !actions.apply(R"({"type":"action","key":"store","phase":"press"})") &&
              !actions.apply(R"({"type":"action","key":"tuner","phase":"release"})") &&
              calls.empty(), "undeclared action or phase reached the target");
        check(actions.apply(R"({"type":"action","key":"store","phase":"invoke"})"), "invoke did not reach target");
        check(actions.apply(R"({"type":"action","key":"tuner","phase":"press"})") &&
              actions.apply(R"({"type":"action","key":"tuner","phase":"press"})") && actions.activeActions() == 1,
              "duplicate press did not share one release");
        tunerAvailable = false;
        actions.cancel();
        check(!actions.activeActions() && calls == std::vector<std::pair<std::string, ActionPhase>>{
            {"store", ActionPhase::invoke}, {"tuner", ActionPhase::press}, {"tuner", ActionPhase::release}},
            "bridge cancellation did not balance momentary actions");
    }
    {
        std::optional<ProgramAddress> active;
        web::ParameterBridge memory({fields,
            [&](ParamId) { return hostValue; },
            [](ParamId) { return true; },
            [](ParamId, double, bool) { return true; },
            [](ParamId) { return true; },
            [&] { return ProgramCatalog{{{3, "Bank D", {{5, "Dark Room"}}}}, active}; },
            [&](ProgramAddress address) { active = address; return true; }
        }, {});
        check(json::read(memory.snapshot())["programs"]["active"].isNull(), "empty patch selection was not reported");
        check(!memory.apply(R"({"type":"recall-program","bank":3,"program":0})") && !active,
              "empty slot reached the host");
        check(memory.apply(R"({"type":"recall-program","bank":3,"program":5})") && active == ProgramAddress{3, 5},
              "a program-only bridge failed to recall the declared slot");
        const auto snapshot = json::read(memory.snapshot());
        const auto& selection = snapshot["programs"]["active"];
        const auto* bank = selection["bank"].unsignedInteger();
        const auto* program = selection["program"].unsignedInteger();
        check(snapshot["parameters"].array()->empty() && bank && *bank == 3 && program && *program == 5,
              "program-only snapshot has wrong selection");
    }
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
