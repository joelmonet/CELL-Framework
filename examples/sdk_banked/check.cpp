// Original Cell Framework implementation; see LICENSE.
#include "gain.hpp"
#include <cell/web_parameter_bridge.hpp>
#include <iostream>

namespace {
void expect(bool accepted, const char* reason) {
    if (!accepted) throw std::runtime_error(reason);
}
struct Dump : cell::midi::Output {
    std::array<std::uint8_t, 13> bytes{};
    std::uint32_t frame{};
    unsigned calls{};
    bool push(cell::MidiEvent event) noexcept override {
        if (event.sysEx.size() != bytes.size()) return false;
        std::copy(event.sysEx.begin(), event.sysEx.end(), bytes.begin());
        frame = event.frame;
        ++calls;
        return true;
    }
};
}

int main(int argc, char** argv) try {
    using namespace cell;
    using example::gainId;
    Engine engine(plugin::descriptor(), plugin::createProcessor());
    engine.prepare(48000, 128, 2);
    constexpr std::array fields{gainId};
    constexpr std::array actions{web::ActionBinding{"factory-recall", web::ActionMode::invoke}};
    web::ParameterBridge bridge(engine, fields, true, actions);
    if (argc == 2 && std::string_view(argv[1]) == "--parameters") {
        std::cout << bridge.snapshot() << '\n';
        return 0;
    }
    expect(bridge.apply(R"({"type":"recall-program","bank":2,"program":4})"), "Panel recall failed");
    expect(engine.parameters().get(gainId) == -28, "Panel recalled the wrong gain");
    const auto state = engine.saveState();
    expect(bridge.apply(R"({"type":"action","key":"factory-recall","phase":"invoke"})"),
           "Panel hold endpoint failed");
    expect(engine.parameters().get(gainId) == 0 && engine.loadState(state) &&
           engine.parameters().get(gainId) == -28, "Factory recall or state restore failed");

    constexpr std::array<std::uint8_t, 8> foreign{0xf0,0x7d,0x43,0x46,1,0,0,0xf7};
    constexpr std::array<std::uint8_t, 8> recall{0xf0,0x7d,0x43,0x47,1,1,2,0xf7};
    constexpr std::array<std::uint8_t, 7> query{0xf0,0x7d,0x43,0x47,2,1,0xf7};
    constexpr std::array<std::uint8_t, 13> expected{0xf0,0x7d,0x43,0x47,3,1,12,14,16,18,20,22,0xf7};
    AudioBuffer<float> buffer(2, 128);
    Dump dump;
    const auto unrelated = MidiEvent::systemExclusive(4, foreign);
    engine.process(buffer.block(), {}, {}, {&unrelated, 1}, {}, &dump);
    expect(engine.parameters().get(gainId) == -28 && !dump.calls, "Foreign SysEx changed the fixture");
    const std::array events{MidiEvent::systemExclusive(7, recall), MidiEvent::systemExclusive(19, query)};
    engine.process(buffer.block(), {}, {}, events, {}, &dump);
    expect(engine.parameters().get(gainId) == -44, "Recognized SysEx recall did not reach the processor");
    expect(dump.calls == 1 && dump.frame == 19 && dump.bytes == expected, "Bank dump bytes or timing differ");
    const auto snapshot = json::read(bridge.snapshot());
    const auto* active = snapshot["programs"]["active"]["program"].number();
    expect(active && *active == 2,
           "SysEx recall did not update the panel snapshot");
} catch (const std::exception& failure) {
    std::cerr << failure.what() << '\n';
    return 1;
}
