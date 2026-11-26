#include <cell/web_parameter_bridge.hpp>
#include <cell/web_meter.hpp>
#include <iostream>
#include <thread>
using namespace cell;
namespace {
void check(bool v,const char* text){if(!v)throw std::runtime_error(text);}
class Gain final:public Processor{void prepare(const ProcessSpec&)override{}void reset()noexcept override{}void process(AudioBlock<float>,const ParameterBank&,const Transport&,std::span<const MidiEvent>)noexcept override{}};

class Librarian final : public Processor, public ParameterPresentationSource, public EditorStatusSource,
                         public WritableProgramMemory, public StateExtension, public web::ActionTarget {
public:
    void prepare(const ProcessSpec&) override {}
    void reset() noexcept override {}
    void process(AudioBlock<float>, const ParameterBank&, const Transport&, std::span<const MidiEvent>) noexcept override {}
    std::optional<ParameterPresentation> parameterPresentation(ParamId id) const override {
        if (id == stableId("mode")) {
            ParameterPresentation result{{id,"mode",revision ? "New algorithm" : "Algorithm","",0,3,0,1}};
            result.field.choices = {"Mono","Tape","Symphonic","Studio"};
            result.displayCodes = {"Mn","tP","Sy","St"};
            result.detentLabels = result.field.choices;
            result.readOnly = readOnly;
            return result;
        }
        if (id == stableId("hidden")) return ParameterPresentation{{id,"hidden","Hidden","",0,1,0},true};
        return {};
    }
    std::uint64_t parameterPresentationRevision() const noexcept override { return revision; }
    std::vector<EditorStatus> editorStatus() const override {
        ++statusReads;
        return {{"patch","Patch name",name,"info"}};
    }
    std::uint64_t editorStatusRevision() const noexcept override { return revision; }
    ProgramCatalog programs(const ParameterBank&) const override {
        return {{{0,"User",{{0,name}}}}, ProgramAddress{0,0}};
    }
    bool recallProgram(Engine&, ProgramAddress address) override { return address == ProgramAddress{0,0}; }
    std::uint64_t catalogRevision() const noexcept override { return revision; }
    ProgramWriteResult store(ProgramAddress address, std::span<const std::byte> data) override {
        if (address != ProgramAddress{0,0} || data.size() != 2) return {false,"Invalid slot data"};
        name = "Stored";
        ++revision;
        return {true,{}};
    }
    ProgramWriteResult rename(ProgramAddress address, std::string_view text) override {
        if (address != ProgramAddress{0,0}) return {false,"Invalid slot"};
        name = text;
        ++revision;
        return {true,{}};
    }
    ProgramWriteResult initialize(ProgramAddress address) override { return rename(address, "Init"); }
    std::vector<std::byte> saveProductState(const ParameterBank&) const override {
        const auto bytes=std::as_bytes(std::span(name));
        return {bytes.begin(),bytes.end()};
    }
    bool loadProductState(std::span<const std::byte> bytes, ParameterBank&) override {
        name.assign(reinterpret_cast<const char*>(bytes.data()),bytes.size());
        ++revision;
        return true;
    }
    bool supportsWebAction(std::string_view key, web::ActionMode) const noexcept override { return key == "import"; }
    bool webAction(Engine&, std::string_view, web::ActionPhase) override { return false; }
    web::ActionResult webActionWithArgument(Engine&, std::string_view, web::ActionPhase,
                                            std::string_view argument) override {
        return {false, argument == "bank" ? "Invalid checksum" : "Invalid argument"};
    }
    std::uint64_t revision{};
    bool readOnly{};
    std::string name{"Init"};
    mutable int statusReads{};
};

void magicstompBridge() {
    const std::array ids{stableId("mode"),stableId("hidden")};
    const std::array<Parameter,2> fields{{
        {ids[0],"mode","Mode","",0,1,0}, {ids[1],"hidden","Hidden","",0,1,0}}};
    const PluginDescriptor descriptor{"cell.web.librarian","Librarian","Cell","1",2,fields};
    auto owner=std::make_unique<Librarian>();
    auto* processor=owner.get();
    Engine engine(descriptor,std::move(owner));
    const std::array actions{web::ActionBinding{"import",web::ActionMode::momentary}};
    web::ParameterBridge bridge(engine,ids,true,actions);
    check(bridge.apply(R"({"type":"parameter","key":"mode","value":2})") &&
          engine.parameters().get(ids[0])==2.0/3,"Presented range did not map back to the storage domain");
    const auto initial= json::read(bridge.snapshot());
    const auto& item=initial["parameters"].array()->front();
    check(initial["parameters"].array()->size()==1 && item["name"].text()=="Algorithm" &&
          item["text"].text()=="Symphonic" && *item["value"].number()==2 &&
          item["display"]["codes"].array()->at(2).text()=="Sy","Live metadata or hidden filtering failed");
    (void)bridge.snapshot();
    check(processor->statusReads==1,"Unchanged editor status was collected again");
    std::string wire;
    const auto record=[&](const std::string& message){wire=message;return true;};
    auto full=[&]{return json::read(wire);};
    check(bridge.updateParameters(record)&&full()["type"].text()=="parameters","Initial delta subscriber missed its snapshot");
    engine.parameters().set(ids[0],1);
    check(bridge.updateParameters(record)&&full()["type"].text()=="parameter-delta" &&
          full()["parameters"].array()->size()==1 && full()["status"].isNull(),
          "Delta duplicated status or failed to send the changed parameter");
    bridge.resetParameterUpdates();
    check(bridge.updateParameters(record)&&full()["type"].text()=="parameters","Force-resync did not send a full snapshot");
    check(bridge.apply(R"({"type":"store-program","bank":0,"program":0,"bytes":[77,83]})") &&
          *json::read(bridge.takeReply())["ok"].boolean(),"Bridge could not store program data");
    check(bridge.apply(R"({"type":"rename-program","bank":0,"program":0,"name":"Vintage delay"})"),"Bridge could not rename");
    const auto saved=engine.saveState();
    check(bridge.apply(R"({"type":"initialize-program","bank":0,"program":0})") && engine.loadState(saved) &&
          json::read(bridge.snapshot())["status"].array()->front()["text"].text()=="Vintage delay",
          "Product state did not preserve a librarian rename");
    processor->readOnly=true;
    ++processor->revision;
    check(*json::read(bridge.snapshot())["parameters"].array()->front()["readonly"].boolean() &&
          !bridge.apply(R"({"type":"begin","key":"mode"})"),"A read-only presented lane acquired a gesture");
    check(!bridge.apply(R"({"type":"action","key":"import","phase":"press","argument":"bank","request":"x"})") &&
          bridge.activeActions()==0 && json::read(bridge.takeReply())["message"].text()=="Invalid checksum",
          "Failed action did not report its result or retained a gesture");

    std::vector<web::EditorFileRequest> files;
    bridge.setFileEndpoint({".mstbank"},[&](auto request){files.push_back(std::move(request));return true;});
    (void)bridge.apply(R"({"type":"open-file","request":"refused","filters":[".txt"]})");
    const auto open = [&] {(void)bridge.apply(R"({"type":"open-file","request":"bank","filters":[".mstbank"]})");};
    check(files.empty(),"File endpoint opened a disallowed file type");
    open();
    bridge.finishFile(true,{},{});
    check(*json::read(bridge.takeReply())["cancelled"].boolean(),"File chooser cancellation was lost");
    open();
    const std::vector<std::byte> large(web::maxEditorFileBytes+1);
    bridge.finishFile(false,"large.mstbank",large);
    check(!*json::read(bridge.takeReply())["ok"].boolean(),"Oversize file was returned to the page");
    open();
    const std::array data{std::byte{77},std::byte{83}};
    bridge.finishFile(false,"demo.mstbank",data);
    check(json::read(bridge.takeReply())["bytes"].text()=="TVM=","File chooser changed the selected bytes");
    (void)bridge.apply(R"({"type":"open-file","request":"save","filters":[".mstbank"],"save":true,"bytes":"TVM="})");
    check(files.size()==4 && files.back().save && std::ranges::equal(files.back().bytes,data),"File export did not decode bytes");
}
}
int main(){try{
    magicstompBridge();
    const std::array<Parameter,2> fields{{{stableId("gain"),"gain","Gain","dB",-60,12,0,.1},{stableId("private"),"private","Hidden","",0,1,0,1}}};
    PluginDescriptor descriptor{"cell.web.test","Web test","Cell","1",2,fields};Engine engine(descriptor,std::make_unique<Gain>());engine.prepare(48000,64);
    const std::array allowed{stableId("gain")};web::ParameterBridge bridge(engine,allowed);
    const auto before=json::read(bridge.snapshot());check(before["parameters"].array()->size()==1,"bridge exported unlisted parameters");
    {
        const std::array watched{stableId("gain"), stableId("private")};
        const std::array readOnly{watched[1]};
        web::ParameterBridge observed(engine, watched, true, {}, readOnly);
        check(observed.allowsParameter("gain", true) && observed.allowsParameter("private", false) &&
              !observed.allowsParameter("private", true) && !observed.allowsParameter("missing", false),
              "Live binding checks changed the compiled parameter access");
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
        check(actions.allowsAction("store", ActionMode::invoke) &&
              !actions.allowsAction("store", ActionMode::momentary) &&
              !actions.allowsAction("private", ActionMode::invoke), "Live action checks changed the compiled modes");
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
    const auto combined = json::read(web::OutputMeter::snapshot({.25f,.125f},
        web::OutputMeter::Levels{.5f,.25f}, 6.f));
    const auto& signals = *combined["meters"].array();
    check(signals.size() == 5 && signals[2]["key"].text() == "input-peak" &&
          *signals[2]["value"].number() == -6.0 && signals[3]["key"].text() == "input-rms" &&
          signals[4]["key"].text() == "gain-reduction" && *signals[4]["value"].number() == 6.0,
          "Combined meter readings lost input level or gain reduction");
    std::cout<<"PASS: web parameter allow-list, bounds, gestures, cancellation and descriptor snapshot\n";
}catch(const std::exception& e){std::cerr<<e.what()<<'\n';return 1;}}
