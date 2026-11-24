// Original Cell Framework implementation; see LICENSE.
#include "../examples/portable_graph/processor.hpp"
#include "allocation_watch.hpp"
#include <cell/json.hpp>
#include <array>
#include <iostream>

namespace {

using namespace cell;
constexpr auto source = R"({
    "schema":"cell.graph.v1",
    "sampleRate":48000,
    "nodes":[
        {"id":"tone","type":"lowpass","brightness":0.0,"minHz":1000,"maxHz":8000},
        {"id":"level","type":"gain","gainDb":-6}
    ],
    "connections":[
        {"from":"tone","to":"output"},
        {"from":"input","to":"level"},
        {"from":"level","to":"tone"}
    ],
    "automation":[
        {"sample":7,"node":"level","parameter":"gainDb","value":0},
        {"sample":7,"node":"tone","parameter":"brightness","value":1},
        {"sample":16,"node":"level","parameter":"gainDb","value":-6}
    ]
})";

void check(bool ok, const char* message) {
    if (!ok)
        throw std::runtime_error(message);
}

void rejects(std::string_view from, std::string_view to) {
    std::string changed(source);
    changed.replace(changed.find(from), from.size(), to);
    try {
        (void)graph::Document::parse(changed);
    } catch (const std::invalid_argument&) {
        return;
    }
    throw std::runtime_error("Invalid graph was accepted");
}

void run() {
    const auto document = graph::Document::parse(source);
    const auto saved = graph::Document::parse(document.serialize());
    check(saved.serialize() == document.serialize(), "Serialization drift");
    rejects("cell.graph.v1", "cell.graph.v4");
    rejects(R"("to":"output")", R"("to":"level")");
    rejects(R"("to":"tone")", R"("to":"unknown")");
    rejects(R"("sample":16)", R"("sample":6)");
    rejects(R"("sample":16)", R"("sample":16.0)");
    rejects(R"("value":1)", R"("value":2)");
    rejects(R"("parameter":"brightness")", R"("parameter":"gainDb")");

    graph::Player player(document, {48000, 32, 2});
    Engine engine(example::graphDescriptor, std::make_unique<example::GraphProcessor>(saved));
    engine.prepare(48000, 32, 2);

    std::array<float, 32> a, b, c, d;
    a.fill(.125f);
    b.fill(-.25f);
    c = a;
    d = b;
    float* headless[]{a.data(), b.data()};
    float* hosted[]{c.data(), d.data()};
    const AudioBlock<float> block{hosted, 2, 32};
    cell_test::audio = true;
    const bool rejected = !player.process({headless, 1, 7});
    const bool processed = player.process({headless, 2, 32});
    engine.process(block.slice(0, 7));
    engine.process(block.slice(7, 9));
    engine.process(block.slice(16, 16));
    cell_test::audio = false;
    check(rejected && processed && a == c && b == d, "Entry points differ across block boundaries");
    check(cell_test::allocations == 0 && cell_test::deletions == 0, "Audio processing allocated or freed memory");

    dsp::Biquad reference;
    reference.coefficients(dsp::BiquadCoefficients::lowpass(48000, 1000));
    for (unsigned frame = 0; frame < a.size(); ++frame) {
        if (frame == 7)
            reference.coefficients(dsp::BiquadCoefficients::lowpass(48000, 8000));
        const auto expected = reference.process(.125f * dbToGain(frame >= 7 && frame < 16 ? 0 : -6));
        check(a[frame] == expected, "Wrong audio or automation sample");
    }

    player.reset();
    c.fill(.125f);
    d.fill(-.25f);
    check(player.process(block) && a == c && b == d, "Reset did not reproduce render");
}

void liveEdits() {
    const auto original = graph::Document::parse(source);
    auto reorderedText = original.serialize();
    const auto connections = reorderedText.find(R"("connections":)");
    const auto start = reorderedText.find('[', connections);
    const auto end = reorderedText.find(']', start);
    reorderedText.replace(start, end - start + 1,
        R"([{"from":"input","to":"tone"},{"from":"tone","to":"level"},{"from":"level","to":"output"}])");
    const auto reordered = graph::Document::parse(reorderedText);
    graph::LiveSession session(original, {48000, 32, 1}, 8);
    check(!session.undo(), "Initial document should have no undo");

    std::array<float, 32> samples{};
    float* channels[]{samples.data()};
    auto play = [&](unsigned frames) {
        samples.fill(.125f);
        cell_test::audio = true;
        const bool ok = session.process({channels, 1, frames});
        cell_test::audio = false;
        check(ok, "Live graph rejected a valid block");
    };

    dsp::Biquad reference;
    reference.coefficients(dsp::BiquadCoefficients::lowpass(48000, 1000));
    auto verify = [&](unsigned start, unsigned frames) {
        for (unsigned i = 0; i < frames; ++i) {
            const auto sample = start + i;
            if (sample == 4)
                reference.coefficients(dsp::BiquadCoefficients::lowpass(48000, std::sqrt(8000000.0)));
            if (sample == 7)
                reference.coefficients(dsp::BiquadCoefficients::lowpass(48000, 8000));
            check(samples[i] == reference.process(.125f * dbToGain(sample < 7 || sample >= 16 ? -6 : 0)),
                  "Live brightness lost filter state or automation timing");
        }
    };
    play(3);
    verify(0, 3);
    check(session.nextSample() == 3, "Playback clock did not advance");
    check(session.setBrightness("tone", .5, 4) && session.updatePending(), "Brightness point was not queued");
    play(9);
    verify(3, 9);
    check(!session.updatePending(), "Brightness point was not adopted");
    const auto edited = session.document().serialize();

    check(session.replace(reordered), "Reorder failed");
    play(4);
    check(!session.updatePending(), "Reorder was not adopted");
    dsp::Biquad replacement;
    replacement.coefficients(dsp::BiquadCoefficients::lowpass(48000, 8000));
    for (unsigned i = 0; i < 4; ++i) {
        const float old = reference.process(.125f);
        const float next = replacement.process(.125f);
        check(samples[i] == std::lerp(old, next, float(i + 1) / 8.0f), "Reorder fade envelope changed");
    }
    check(session.undo(), "Undo failed");
    check(session.document().serialize() == edited, "Undo restored the wrong document");
    play(4);
    check(session.updatePending(), "Undo interrupted an active fade");
    for (unsigned i = 0; i < 4; ++i) {
        const float old = reference.process(.125f * dbToGain(-6));
        const float next = replacement.process(.125f) * dbToGain(-6);
        check(samples[i] == std::lerp(old, next, float(i + 5) / 8.0f),
              "Automation failed to advance through the crossfade");
    }
    play(8);
    check(!session.updatePending() && session.nextSample() == 28, "Undo disrupted playback position");
    check(session.reclaim() > 0, "Old graphs were not retired");
    check(cell_test::allocations == 0 && cell_test::deletions == 0, "Live edit allocated or freed on audio");

    try {
        session.setBrightness("level", .5, 32);
        throw std::runtime_error("Invalid brightness target accepted");
    } catch (const std::invalid_argument&) {}
    check(!session.updatePending() && session.document().serialize() == edited,
          "Invalid edit changed the live graph");

    const auto reopened = graph::Document::parse(session.document().serialize());
    graph::Player offline(reopened, {48000, 32, 1});
    samples.fill(.125f);
    reference.reset();
    reference.coefficients(dsp::BiquadCoefficients::lowpass(48000, 1000));
    check(offline.process({channels, 1, 32}), "Reopened graph could not render");
    verify(0, 32);

    // Fill the exchange without an audio consumer. A rejected edit must
    // leave the latest accepted document and undo point available.
    graph::LiveSession busy(original, {48000, 32, 1}, 0);
    for (unsigned i = 0; i < 3; ++i)
        check(busy.setBrightness("tone", .1 * i, i), "Unexpected exchange backpressure");
    const auto queued = busy.document().serialize();
    check(!busy.replace(reordered) && busy.document().serialize() == queued,
          "Backpressure changed the accepted document");
    check(busy.process({channels, 1, 1}) && busy.undo(), "Queued edit could not be undone");
    const auto expected = original.withBrightness("tone", 0, 0).withBrightness("tone", .1, 1);
    check(busy.document().serialize() == expected.serialize(), "Backpressure changed undo history");
}

void parallelGraph() {
    constexpr auto text = R"({
        "schema":"cell.graph.v2", "sampleRate":48000,
        "nodes":[
            {"id":"mix","type":"gain","gainDb":-3},
            {"id":"tone","type":"lowpass","brightness":0,"minHz":1000,"maxHz":8000},
            {"id":"dry","type":"gain","gainDb":-6}
        ],
        "connections":[
            {"from":"input","to":"dry"}, {"from":"input","to":"tone"},
            {"from":"tone","to":"mix"}, {"from":"dry","to":"mix"},
            {"from":"mix","to":"output"}, {"from":"input","to":"output"}
        ],
        "automation":[{"sample":3,"node":"tone","parameter":"brightness","value":1}]
    })";
    const auto document = graph::Document::parse(text);
    auto reordered = *json::read(text).object();
    for (const auto* key : {"nodes", "connections"}) {
        auto entries = *reordered.at(key).array();
        std::reverse(entries.begin(), entries.end());
        reordered.at(key) = json::Value(std::move(entries));
    }
    const auto alternate = graph::Document::parse(json::write(json::Value(reordered)));
    graph::Player player(document, {48000, 9, 2});
    graph::Player split(alternate, {48000, 4, 2});
    std::array<float, 11> left, right, otherLeft, otherRight;
    for (unsigned i = 0; i < left.size(); ++i) {
        left[i] = float(i) / 32;
        right[i] = -float(i) / 64;
    }
    otherLeft = left;
    otherRight = right;
    float* channels[]{left.data(), right.data()};
    float* other[]{otherLeft.data(), otherRight.data()};
    const AudioBlock<float> block{channels, 2, 9, 1};
    const AudioBlock<float> splitBlock{other, 2, 9, 1};
    cell_test::audio = true;
    const auto ok = player.process(block) && split.process(splitBlock.slice(0, 3)) &&
        split.process(splitBlock.slice(3, 4)) && split.process(splitBlock.slice(7, 2));
    cell_test::audio = false;
    check(ok && left == otherLeft && right == otherRight, "Parallel render depends on block or declaration order");
    check(cell_test::allocations == 0 && cell_test::deletions == 0, "Parallel graph allocated or freed on audio");

    for (unsigned c = 0; c < 2; ++c) {
        dsp::Biquad lowpass;
        lowpass.coefficients(dsp::BiquadCoefficients::lowpass(48000, 1000));
        for (unsigned i = 0; i < 9; ++i) {
            if (i == 3)
                lowpass.coefficients(dsp::BiquadCoefficients::lowpass(48000, 8000));
            const auto input = c == 0 ? float(i + 1) / 32 : -float(i + 1) / 64;
            const auto expected = input + (input * dbToGain(-6) + lowpass.process(input)) * dbToGain(-3);
            check(block.channel(c)[i] == expected, "Parallel merge or automation produced the wrong sample");
        }
    }
    check(left.front() == 0 && left.back() == 10.f / 32, "Parallel graph wrote outside its block");

    const auto rejected = [&](std::string_view from, std::string_view to) {
        std::string changed(text);
        changed.replace(changed.find(from), from.size(), to);
        try {
            (void)graph::Document::parse(changed);
        } catch (const std::invalid_argument&) {
            return;
        }
        throw std::runtime_error("Invalid parallel graph was accepted");
    };
    rejected(R"({"from":"input","to":"tone"})", R"({"from":"mix","to":"tone"})");
    rejected(R"({"from":"input","to":"tone"})", R"({"from":"input","to":"dry"})");
    rejected(R"({"from":"tone","to":"mix"},)", "");
    rejected(R"({"from":"input","to":"tone"},)", "");
    try {
        graph::Player oversized(document, {48000, 100000, 64});
        throw std::runtime_error("Parallel graph exceeded its scratch limit");
    } catch (const std::invalid_argument&) {}
}

void liveParallelRouting() {
    constexpr auto text = R"({
        "schema":"cell.graph.v2", "sampleRate":48000,
        "nodes":[{"id":"branch","type":"gain","gainDb":0}],
        "connections":[{"from":"input","to":"branch"},{"from":"branch","to":"output"},{"from":"input","to":"output"}],
        "automation":[]
    })";
    const auto parallel = graph::Document::parse(text);
    std::string serialText(text);
    constexpr std::string_view direct = R"(,{"from":"input","to":"output"})";
    serialText.erase(serialText.find(direct), direct.size());
    graph::LiveSession session(parallel, {48000, 4, 1}, 4);
    std::array<float, 4> samples{};
    float* channels[]{samples.data()};
    const AudioBlock<float> block{channels, 1, 4};
    samples.fill(.25f);
    check(session.process(block) && samples.back() == .5f, "Parallel input failed to split");
    check(session.replace(graph::Document::parse(serialText)), "Could not queue parallel routing edit");
    samples.fill(.25f);
    cell_test::audio = true;
    const auto ok = session.process(block);
    cell_test::audio = false;
    check(ok && samples == std::array<float, 4>{.4375f, .375f, .3125f, .25f},
          "Routing-only edit failed to crossfade");
    check(session.undo(), "Could not undo routing edit");
    samples.fill(.25f);
    check(session.process(block) && samples.back() == .5f && session.nextSample() == 12,
          "Routing undo failed to restore the mix without interrupting the clock");
    check(cell_test::allocations == 0 && cell_test::deletions == 0, "Parallel live swap allocated or freed on audio");
}

void latencyCompensation() {
    constexpr auto text = R"({
        "schema":"cell.graph.v2", "sampleRate":48000,
        "nodes":[{"id":"slow","type":"delay","frames":3},{"id":"sum","type":"gain","gainDb":0}],
        "connections":[{"from":"input","to":"slow"},{"from":"slow","to":"sum"},
                       {"from":"input","to":"sum"},{"from":"sum","to":"output"},{"from":"input","to":"output"}],
        "automation":[{"sample":5,"node":"sum","parameter":"gainDb","value":-6}]
    })";
    const auto document = graph::Document::parse(text);
    graph::Player whole(document, {48000, 9, 2});
    Engine engine(example::graphDescriptor, std::make_unique<example::GraphProcessor>(document));
    engine.prepare(48000, 4, 2);
    check(document.latency() == 3 && whole.latency() == 3 && engine.processor().latency() == 3,
          "Graph did not report the longest path latency");

    std::array<float, 9> left{}, right{}, otherLeft{}, otherRight{};
    left[0] = .125f;
    left[2] = .25f;
    right[0] = -.25f;
    otherLeft = left;
    otherRight = right;
    float* channels[]{left.data(), right.data()};
    float* other[]{otherLeft.data(), otherRight.data()};
    AudioBlock<float> block{channels, 2, 9};
    AudioBlock<float> split{other, 2, 9};
    cell_test::audio = true;
    const auto ok = whole.process(block);
    engine.process(split.slice(0, 2));
    engine.process(split.slice(2, 4));
    engine.process(split.slice(6, 3));
    cell_test::audio = false;
    check(ok && left == otherLeft && right == otherRight, "Latency compensation changed with block boundaries");
    check(left[0] == 0 && left[1] == 0 && left[2] == 0 && left[3] == .375f &&
          right[3] == -.75f && left[4] == 0 && left[5] == .25f + .5f * dbToGain(-6),
          "Paths did not align at both the internal and final merges");

    whole.reset();
    left.fill(0);
    right.fill(0);
    check(whole.process(block) && std::all_of(left.begin(), left.end(), [](float value) { return value == 0; }),
          "Graph reset leaked old delay history");
    check(cell_test::allocations == 0 && cell_test::deletions == 0, "Latency compensation allocated or freed on audio");

    std::string invalid(text);
    invalid.replace(invalid.find(R"("frames":3)"), 10, R"("frames":0)");
    try {
        (void)graph::Document::parse(invalid);
        throw std::runtime_error("A zero-frame delay node was accepted");
    } catch (const std::invalid_argument&) {}
}

void liveCompensation() {
    constexpr auto text = R"({
        "schema":"cell.graph.v2", "sampleRate":48000,
        "nodes":[{"id":"slow","type":"delay","frames":3}],
        "connections":[{"from":"input","to":"slow"},{"from":"slow","to":"output"},{"from":"input","to":"output"}],
        "automation":[]
    })";
    const auto document = graph::Document::parse(text);
    graph::LiveSession session(document, {48000, 4, 1}, 4);
    std::array<float, 4> samples{};
    float* channels[]{samples.data()};
    AudioBlock<float> block{channels, 1, 4};
    const auto render = [&] {
        samples.fill(.25f);
        cell_test::audio = true;
        const auto ok = session.process(block);
        cell_test::audio = false;
        check(ok, "Delayed live processing rejected a valid block");
    };
    render();
    check(samples.back() == .5f, "Delayed live session did not prime");
    check(session.replace(document), "Could not queue same-topology replacement");
    render();
    check(samples == std::array<float, 4>{.5f, .5f, .5f, .5f},
          "Same-topology replacement dropped delay history");

    std::string changed(text);
    constexpr std::string_view direct = R"(,{"from":"input","to":"output"})";
    changed.erase(changed.find(direct), direct.size());
    check(session.replace(graph::Document::parse(changed)), "Could not queue new routing");
    render();
    check(samples == std::array<float, 4>{.5f, .5f, .5f, .4375f}, "New route faded before it had audio");
    render();
    check(samples == std::array<float, 4>{.375f, .3125f, .25f, .25f}, "Delayed crossfade ended on the wrong sample");

    changed.replace(changed.find(R"("frames":3)"), 10, R"("frames":4)");
    try {
        session.replace(graph::Document::parse(changed));
        throw std::runtime_error("A running session accepted a latency change");
    } catch (const std::invalid_argument&) {}
    check(session.latency() == 3 && session.nextSample() == 16 && !session.updatePending(),
          "Rejected latency change modified the live session");
    check(cell_test::allocations == 0 && cell_test::deletions == 0, "Delay state handoff allocated or freed on audio");
}

void automationControls() {
    const auto document = graph::Document::parse(source);
    const auto controls = document.controls();
    check(controls.size() == 2 && controls[0].node == "level" && controls[0].key == "gainDb" &&
          controls[0].meaning == "level" && controls[0].unit == "dB" &&
          controls[0].minimum == -60 && controls[0].maximum == 12 &&
          controls[0].initial == -6 && !controls[0].mapping, "Wrong gain control metadata");
    const auto& tone = controls[1];
    check(tone.node == "tone" && tone.key == "brightness" && tone.unit == "normalized" &&
          tone.minimum == 0 && tone.maximum == 1 && tone.mapping && tone.mapping->target == "cutoff" &&
          tone.mapping->unit == "Hz" && tone.mapping->map(0) == 1000 && tone.mapping->map(1) == 8000 &&
          std::abs(tone.mapping->map(.5) - std::sqrt(8000000.0)) < 1e-9,
          "Brightness metadata disagrees with the playback mapping");

    constexpr std::array changes{
        graph::AutomationPoint{"level", "gainDb", UINT64_MAX, -3},
        graph::AutomationPoint{"level", "gainDb", 7, -12},
        graph::AutomationPoint{"tone", "brightness", 3, .5},
        graph::AutomationPoint{"level", "gainDb", 7, -9}
    };
    const auto updated = document.withAutomation(changes);
    const auto root = json::read(updated.serialize());
    const auto& points = *root["automation"].array();
    check(points.size() == 7 && *points[0]["sample"].number() == 3 &&
          *points[1]["value"].number() == 0 && *points[3]["value"].number() == -12 &&
          *points[4]["value"].number() == -9 && points.back()["sample"].unsignedInteger() &&
          *points.back()["sample"].unsignedInteger() == UINT64_MAX,
          "Automation order or 64-bit sample positions changed");

    graph::LiveSession session(document, {48000, 16, 1});
    check(session.setAutomation(changes) && session.document().serialize() == updated.serialize(),
          "Live automation batch did not publish as one document");
    const std::array invalid{graph::AutomationPoint{"tone", "brightness", 0, .5},
                             graph::AutomationPoint{"level", "gainDb", 0, 13}};
    try {
        session.setAutomation(invalid);
        throw std::runtime_error("Invalid point did not reject its automation batch");
    } catch (const std::invalid_argument&) {}
    check(session.document().serialize() == updated.serialize() && session.undo() &&
          session.document().serialize() == document.serialize() && !session.undo(),
          "Batch validation changed existing state or split undo");
}

void musicalIntents() {
    constexpr auto text = R"({
        "schema":"cell.graph.v3", "sampleRate":48000,
        "nodes":[
            {"id":"tone","type":"lowpass","brightness":0.25,"minHz":1000,"maxHz":8000},
            {"id":"wet","type":"gain","gainDb":-3}
        ],
        "connections":[{"from":"input","to":"tone"},{"from":"tone","to":"wet"},{"from":"wet","to":"output"}],
        "intents":[{"id":"clarity","name":"Clarity","bindings":[
            {"node":"tone","parameter":"brightness","atZero":0.25,"atOne":0.75},
            {"node":"wet","parameter":"gainDb","atZero":-3,"atOne":-9}
        ]}],
        "automation":[{"sample":3,"node":"wet","parameter":"gainDb","value":0}]
    })";
    const auto original = graph::Document::parse(text);
    check(original.intents().size() == 1 && original.intents().front().id == "clarity" &&
          original.intents().front().name == "Clarity" && original.intents().front().bindings.size() == 2,
          "Missing musical intent metadata");

    const auto composed = original.withIntent("clarity", .5, 3);
    const auto saved = json::read(composed.serialize());
    const auto& timeline = *saved["automation"].array();
    check(timeline.size() == 2 && timeline.back()["intent"].text() == "clarity" &&
          *timeline.back()["value"].number() == .5, "Save flattened the musical gesture");

    const std::array physical{
        graph::AutomationPoint{"tone", "brightness", 3, .5},
        graph::AutomationPoint{"wet", "gainDb", 3, -6}};
    const auto expected = original.withAutomation(physical);
    graph::Player savedPlayer(graph::Document::parse(composed.serialize()), {48000, 8, 1});
    graph::Player physicalPlayer(expected, {48000, 8, 1});
    graph::LiveSession live(original, {48000, 8, 1});
    check(live.setIntent("clarity", .5, 3), "Could not schedule a musical gesture");

    std::array<float, 8> a, b, c;
    a.fill(.25f);
    b = c = a;
    float* pa[]{a.data()};
    float* pb[]{b.data()};
    float* pc[]{c.data()};
    AudioBlock<float> liveBlock{pc, 1, 8};
    cell_test::audio = true;
    const auto ok = savedPlayer.process({pa, 1, 8}) && physicalPlayer.process({pb, 1, 8}) &&
        live.process(liveBlock.slice(0, 3)) && live.process(liveBlock.slice(3, 5));
    cell_test::audio = false;
    check(ok && a == b && b == c, "Intent playback differs from explicit sample-timed automation");
    check(cell_test::allocations == 0 && cell_test::deletions == 0, "Intent expanded on the audio thread");

    const auto rejected = [&](std::string_view from, std::string_view to) {
        std::string changed(text);
        changed.replace(changed.find(from), from.size(), to);
        try {
            (void)graph::Document::parse(changed);
        } catch (const std::invalid_argument&) {
            return;
        }
        throw std::runtime_error("Invalid musical binding was accepted");
    };
    rejected(R"("atOne":0.75)", R"("atOne":1.5)");
    rejected(R"("node":"tone","parameter":"brightness")", R"("node":"missing","parameter":"brightness")");
    rejected(R"("node":"tone","parameter":"brightness")", R"("node":"wet","parameter":"gainDb")");
    try {
        live.setIntent("clarity", 2, 4);
        throw std::runtime_error("Invalid normalized live gesture was accepted");
    } catch (const std::invalid_argument&) {}
    check(live.document().serialize() == composed.serialize() && live.undo() &&
          live.document().serialize() == original.serialize() && !live.undo(),
          "Live gesture validation or undo was not atomic");
}

graph::Document saturationGraph(unsigned factor, bool dry = false) {
    auto text = std::string(R"({
        "schema":"cell.graph.v2","sampleRate":48000,
        "nodes":[{"id":"drive","type":"saturator","driveDb":24,"oversampling":)") + std::to_string(factor) +
        R"(}],"connections":[{"from":"input","to":"drive"},{"from":"drive","to":"output"})";
    if (dry)
        text += R"(,{"from":"input","to":"output"})";
    return graph::Document::parse(text + R"(],"automation":[]})");
}

void saturation() {
    for (const auto factor : {1u, 2u, 4u, 8u, 16u}) {
        const auto graph = saturationGraph(factor);
        check(graph.latency() == 64 - 64 / factor && graph.tail() == graph.latency(),
              "Saturator latency or finite tail disagrees with the FIR");
        const auto controls = graph.controls();
        check(controls.size() == 1 && controls.front().key == "driveDb" &&
              controls.front().unit == "dB" && controls.front().maximum == 36,
              "Wrong saturator control metadata");
    }
    try {
        (void)saturationGraph(3);
        throw std::runtime_error("Unsupported saturator factor was accepted");
    } catch (const std::invalid_argument&) {}

    const auto wet = saturationGraph(4);
    graph::Player wetPlayer(wet, {48000, 128, 1});
    graph::Player dryWet(saturationGraph(4, true), {48000, 128, 1});
    std::array<float, 128> impulse{}, parallel{};
    impulse[0] = parallel[0] = .01f;
    float* wetChannel[]{impulse.data()};
    float* parallelChannel[]{parallel.data()};
    cell_test::audio = true;
    const bool ok = wetPlayer.process({wetChannel, 1, 128}) && dryWet.process({parallelChannel, 1, 128});
    cell_test::audio = false;
    check(ok, "Saturator rejected an impulse block");
    const auto peak = std::max_element(impulse.begin(), impulse.end());
    check(static_cast<std::size_t>(peak - impulse.begin()) == wet.latency(),
          "Saturator impulse center disagrees with declared latency");
    for (std::size_t i = 0; i < impulse.size(); ++i)
        check(parallel[i] == (i == wet.latency() ? .01f + impulse[i] : impulse[i]),
              "Dry signal did not align to the saturator delay");

    // 7 kHz generates a fifth harmonic at 35 kHz, which folds to 13 kHz
    // at a 48 kHz base sample rate. Compare a settled, integer-cycle window.
    const auto alias = [](unsigned factor) {
        graph::Player player(saturationGraph(factor), {48000, 240, 1});
        constexpr double turn = 2 * std::numbers::pi / 48000.;
        std::array<float, 240> samples;
        float* channels[]{samples.data()};
        double real{}, imaginary{};
        for (unsigned frame = 0; frame < 4800; frame += samples.size()) {
            for (unsigned i = 0; i < samples.size(); ++i)
                samples[i] = static_cast<float>(.5 * std::sin(turn * 7000 * (frame + i)));
            check(player.process({channels, 1, static_cast<unsigned>(samples.size())}), "Aliasing test could not render");
            if (frame >= 480) {
                for (unsigned i = 0; i < samples.size(); ++i) {
                    real += samples[i] * std::cos(turn * 13000 * (frame + i));
                    imaginary += samples[i] * std::sin(turn * 13000 * (frame + i));
                }
            }
        }
        return std::hypot(real, imaginary);
    };
    const auto baseAlias = alias(1);
    check(baseAlias > 1 && alias(4) < baseAlias * .2, "Oversampling did not reduce the measured folded harmonic");

    const graph::AutomationPoint edit{"drive", "driveDb", 64, 12};
    graph::LiveSession live(wet, {48000, 64, 1});
    graph::Player reference(wet.withAutomation({&edit, 1}), {48000, 64, 1});
    std::array<float, 64> a, b;
    float* pa[]{a.data()};
    float* pb[]{b.data()};
    for (unsigned i = 0; i < 4; ++i) {
        if (i == 1)
            check(live.setAutomation({&edit, 1}), "Could not schedule saturation drive");
        a.fill(.1f);
        b = a;
        cell_test::audio = true;
        const bool done = live.process({pa, 1, 64}) && reference.process({pb, 1, 64});
        cell_test::audio = false;
        check(done && a == b, "Live saturation drive edit discarded oversampling state");
    }
    check(cell_test::allocations == 0 && cell_test::deletions == 0, "Saturation allocated or freed on audio");
}

void multichannelRouting() {
    constexpr auto text = R"({
        "schema":"cell.graph.v2","sampleRate":48000,
        "nodes":[{"id":"route","type":"channelMap","sources":[1,0,null,3,0,5]}],
        "connections":[{"from":"input","to":"route"},{"from":"route","to":"output"}],
        "automation":[]
    })";
    const auto document = graph::Document::parse(text);
    check(document.requiredChannels() == 6 && document.controls().empty(),
          "Channel routing metadata did not declare a fixed width");
    try {
        graph::Player wrong(document, {48000, 4, 2});
        throw std::runtime_error("Channel map accepted the wrong playback width");
    } catch (const std::invalid_argument&) {}

    graph::Player player(document, {48000, 4, 6});
    graph::LiveSession session(document, {48000, 4, 6}, 2);
    std::array<std::array<float, 4>, 6> samples;
    std::array<float*, 6> channels;
    for (unsigned c = 0; c < channels.size(); ++c)
        channels[c] = samples[c].data();
    const auto fill = [&] {
        for (unsigned c = 0; c < channels.size(); ++c)
            samples[c].fill(float(c + 1));
    };
    const AudioBlock<float> block{channels.data(), 6, 4};
    fill();
    cell_test::audio = true;
    const bool processed = player.process(block);
    cell_test::audio = false;
    check(processed && samples[0][3] == 2 && samples[1][3] == 1 && samples[2][3] == 0 &&
          samples[3][3] == 4 && samples[4][3] == 1 && samples[5][3] == 6,
          "Channel map failed to reorder, duplicate, or silence channels");

    fill();
    check(session.process(block), "Live channel mapping failed");
    std::string updated(text);
    constexpr std::string_view current = "1,0,null,3,0,5";
    updated.replace(updated.find(current), current.size(), "0,1,2,3,4,5");
    check(session.replace(graph::Document::parse(updated)), "Could not queue a channel routing edit");
    fill();
    cell_test::audio = true;
    const bool switched = session.process(block);
    cell_test::audio = false;
    check(switched && samples[0][0] == 1.5f && samples[0][1] == 1 && samples[2][0] == 1.5f &&
          samples[2][1] == 3, "Channel-only live edit did not crossfade");
    check(cell_test::allocations == 0 && cell_test::deletions == 0, "Channel mapping allocated or freed on audio");

    for (const auto invalid : {"1,0,null,3,0,6", "1,0,null,3,0,-1", "1,0,null,3,0,1.5", ""}) {
        std::string changed(text);
        changed.replace(changed.find(current), current.size(), invalid);
        try {
            (void)graph::Document::parse(changed);
            throw std::runtime_error("Invalid channel map was accepted");
        } catch (const std::invalid_argument&) {}
    }
}

void stereoPosition() {
    constexpr auto text = R"({
        "schema":"cell.graph.v3","sampleRate":8000,
        "nodes":[{"id":"image","type":"pan","source":1,"pan":-1}],
        "connections":[{"from":"input","to":"image"},{"from":"image","to":"output"}],
        "intents":[{"id":"position","name":"Position","bindings":[
            {"node":"image","parameter":"pan","atZero":-1,"atOne":1}]}],
        "automation":[{"sample":13,"intent":"position","value":1}]
    })";
    const auto document = graph::Document::parse(text);
    const auto fields = document.controls();
    check(document.azimuthLayouts().empty(), "Stereo pan reported a surround speaker layout");
    check(document.requiredChannels() == 2 && fields.size() == 1 &&
          fields.front().key == "pan" && fields.front().meaning == "position" &&
          fields.front().minimum == -1 && fields.front().maximum == 1 && document.latency() == 0,
          "Stereo position metadata is incorrect");

    graph::Player player(document, {8000, 128, 2});
    graph::LiveSession live(document, {8000, 128, 2});
    std::array<float, 128> left, right, splitLeft, splitRight;
    left.fill(-.75f);
    right.fill(.5f);
    splitLeft = left;
    splitRight = right;
    float* wholeChannels[]{left.data(), right.data()};
    float* splitChannels[]{splitLeft.data(), splitRight.data()};
    const AudioBlock<float> split{splitChannels, 2, 128};

    cell_test::audio = true;
    const auto started = player.process({wholeChannels, 2, 128}) && live.process(split.slice(0, 50));
    cell_test::audio = false;
    check(started && live.replace(document), "Stereo position render or prepared swap failed");
    cell_test::audio = true;
    const auto finished = live.process(split.slice(50, 78));
    cell_test::audio = false;
    check(finished && left == splitLeft && right == splitRight, "Stereo movement changed across a live swap or block boundary");
    check(left[12] == .5f && right[12] == 0 && left[13] < .5f && right[13] > 0 &&
          left[91] > 0 && left[92] == 0 && right[92] == .5f,
          "Stereo movement began or settled at the wrong sample");
    for (unsigned i = 0; i < left.size(); ++i)
        check(std::abs(left[i] * left[i] + right[i] * right[i] - .25f) < 1e-6f,
              "Stereo movement changed power");
    check(cell_test::allocations == 0 && cell_test::deletions == 0, "Stereo movement allocated or freed on audio");

    try {
        graph::Player mono(document, {8000, 64, 1});
        throw std::runtime_error("Stereo panner accepted a mono layout");
    } catch (const std::invalid_argument&) {}
    for (const auto& bad : {std::pair{R"("source":1)", R"("source":2)"},
                            std::pair{R"("pan":-1)", R"("pan":-2)"}}) {
        std::string altered(text);
        altered.replace(altered.find(bad.first), std::string_view(bad.first).size(), bad.second);
        try {
            (void)graph::Document::parse(altered);
            throw std::runtime_error("Invalid stereo position was accepted");
        } catch (const std::invalid_argument&) {}
    }
}

void surroundPosition() {
    constexpr auto text = R"({
        "schema":"cell.graph.v3","sampleRate":8000,
        "nodes":[{"id":"image","type":"azimuth","source":0,"azimuth":170,
                  "speakers":[-30,30,0,null,-110,110]}],
        "connections":[{"from":"input","to":"image"},{"from":"image","to":"output"}],
        "intents":[{"id":"position","name":"Position","bindings":[
            {"node":"image","parameter":"azimuth","atZero":170,"atOne":-170}]}],
        "automation":[{"sample":13,"intent":"position","value":1}]
    })";
    const auto document = graph::Document::parse(text);
    const auto controls = document.controls();
    const auto layouts = document.azimuthLayouts();
    const std::vector<std::optional<float>> expected{-30, 30, 0, std::nullopt, -110, 110};
    check(layouts.size() == 1 && layouts.front().node == "image" && layouts.front().sourceChannel == 0 &&
          layouts.front().speakerDegrees == expected, "Surround layout metadata lost output channel order");
    check(document.requiredChannels() == 6 && document.latency() == 0 && controls.size() == 1 &&
          controls.front().key == "azimuth", "Surround position metadata is incorrect");

    graph::Player player(document, {8000, 128, 6});
    graph::LiveSession live(document, {8000, 128, 6});
    std::array<std::array<float, 128>, 6> samples, splitSamples;
    std::array<float*, 6> channels, splitChannels;
    for (unsigned c = 0; c < channels.size(); ++c) {
        samples[c].fill(c == 0 ? .5f : -.75f);
        splitSamples[c] = samples[c];
        channels[c] = samples[c].data();
        splitChannels[c] = splitSamples[c].data();
    }
    const AudioBlock<float> split{splitChannels.data(), 6, 128};

    cell_test::audio = true;
    const auto started = player.process({channels.data(), 6, 128}) && live.process(split.slice(0, 50));
    cell_test::audio = false;
    check(started && live.replace(document), "Surround render or prepared swap failed");
    cell_test::audio = true;
    const auto finished = live.process(split.slice(50, 78));
    cell_test::audio = false;
    check(finished && samples == splitSamples, "Surround movement changed across a live swap");
    const auto& rearLeft = samples[4];
    const auto& rearRight = samples[5];
    check(rearRight[12] > rearLeft[12] && rearLeft[13] > rearLeft[12] &&
          std::abs(rearLeft[52] - rearRight[52]) < 1e-6f &&
          rearLeft[92] > rearRight[92] && rearLeft[92] == rearLeft[127],
          "Surround movement across the rear seam had incorrect timing or direction");
    for (unsigned i = 0; i < rearLeft.size(); ++i) {
        check(samples[0][i] == 0 && samples[1][i] == 0 && samples[2][i] == 0 && samples[3][i] == 0,
              "Surround movement traveled outside the rear speaker pair");
        check(std::abs(rearLeft[i] * rearLeft[i] + rearRight[i] * rearRight[i] - .25f) < 1e-6f,
              "Surround movement changed power");
    }
    check(cell_test::allocations == 0 && cell_test::deletions == 0, "Surround movement allocated or freed on audio");

    const auto configured = [](std::string_view interpolation, std::string_view graphText) {
        std::string edited(graphText);
        const auto end = edited.find(R"("atOne":-170)") + std::string_view(R"("atOne":-170)").size();
        edited.insert(end, interpolation);
        return graph::Document::parse(edited);
    };
    const auto arc = configured(R"(,"interpolation":"shortestArc")", text);
    const auto renderMidpoint = [&](const graph::Document& source) {
        const auto scheduled = source.withIntent("position", .5, 13);
        graph::Player midpoint(graph::Document::parse(scheduled.serialize()), {8000, 128, 6});
        for (unsigned c = 0; c < channels.size(); ++c)
            samples[c].fill(c == 0 ? .5f : 0.0f);
        check(midpoint.process({channels.data(), 6, 128}), "Could not render surround intent midpoint");
        return std::array{samples[2].back(), rearLeft.back(), rearRight.back()};
    };
    const auto midpoint = renderMidpoint(arc);
    check(midpoint[0] == 0 && midpoint[1] > 0 && midpoint[1] == midpoint[2],
          "Shortest-arc intent midpoint did not stay behind the listener");
    check(renderMidpoint(document) == std::array{.5f, 0.0f, 0.0f},
          "Existing linear azimuth binding changed its midpoint");
    for (const auto* invalid : {R"(,"interpolation":null)", R"(,"interpolation":"clockwise")"}) {
        try {
            (void)configured(invalid, text);
            throw std::runtime_error("Invalid intent interpolation was accepted");
        } catch (const std::invalid_argument&) {}
    }

    constexpr std::string_view layout = "[-30,30,0,null,-110,110]";
    for (const auto* invalid : {"[-180,180,0,null,-110,110]", "[-30,30,0,null,null,null]"}) {
        std::string altered(text);
        altered.replace(altered.find(layout), layout.size(), invalid);
        try {
            (void)graph::Document::parse(altered);
            throw std::runtime_error("Invalid surround layout was accepted");
        } catch (const std::invalid_argument&) {}
    }
}

void listenerOrientation() {
    const auto scene = graph::Document::parse(R"({
        "schema":"cell.graph.v3","sampleRate":8000,"listenerYaw":0,"speakerLayout":[-90,0,90,180],
        "nodes":[
            {"id":"a","type":"azimuth","source":0,"azimuth":0},
            {"id":"b","type":"azimuth","source":1,"azimuth":0,"distanceMeters":2}],
        "connections":[{"from":"input","to":"a"},{"from":"input","to":"b"},
                       {"from":"a","to":"output"},{"from":"b","to":"output"}],
        "intents":[],"automation":[]
    })");
    const std::array trajectory{graph::ListenerYawPoint{96, 90},
                                graph::ListenerYawPoint{8, -90},
                                graph::ListenerYawPoint{8, 90},
                                graph::ListenerYawPoint{40, -90},
                                graph::ListenerYawPoint{40, 90}};
    const auto edited = scene.withListenerYaw(trajectory);
    const auto document = graph::Document::parse(edited.serialize());
    check(document.listenerYaw() == 0, "Listener orientation metadata was lost");
    graph::Player player(document, {8000, 104, 4});
    graph::LiveSession live(scene, {8000, 104, 4});
    const std::array invalid{graph::ListenerYawPoint{8, -90}, graph::ListenerYawPoint{15, 181}};
    try {
        (void)live.setListenerYaw(invalid);
        throw std::runtime_error("Invalid listener trajectory was accepted");
    } catch (const std::invalid_argument&) {}
    check(!live.updatePending(), "Rejected listener trajectory queued a partial edit");
    check(live.setListenerYaw(trajectory), "Live listener automation was rejected");

    std::array<std::array<float, 104>, 4> whole{}, split{};
    std::array<float*, 4> wholeChannels{}, splitChannels{};
    for (unsigned c = 0; c < whole.size(); ++c) {
        whole[c].fill(c < 2 ? .25f : 0.f);
        split[c] = whole[c];
        wholeChannels[c] = whole[c].data();
        splitChannels[c] = split[c].data();
    }
    const AudioBlock<float> splitBlock{splitChannels.data(), 4, 104};
    cell_test::audio = true;
    const bool started = player.process({wholeChannels.data(), 4, 104}) &&
                         live.process(splitBlock.slice(0, 35));
    cell_test::audio = false;
    check(started && live.replace(document), "Listener render or live replacement failed");
    cell_test::audio = true;
    const bool finished = live.process(splitBlock.slice(35, 69));
    cell_test::audio = false;
    check(finished && whole == split, "Listener trajectory changed across live replacement");
    check(whole[0][7] == 0 && whole[1][7] == .375f &&
          std::abs(whole[0][47] - whole[1][47]) < 1e-6f &&
          whole[0][87] == .375f && whole[1][87] == 0,
          "Turning right did not rotate both scene sources to the listener's left");
    const std::array<std::optional<float>, 4> swapped{0, -90, 90, 180};
    const auto retargeted = document.withSpeakerLayout(swapped);
    const auto metadata = json::read(retargeted.serialize());
    check(json::write(metadata["nodes"]) == json::write(json::read(document.serialize())["nodes"]) &&
          json::write(metadata["automation"]) == json::write(json::read(document.serialize())["automation"]),
          "Retargeting changed scene sources or motion");
    graph::Player routed(retargeted, {8000, 104, 4});
    whole[0].fill(.25f);
    whole[1].fill(.25f);
    whole[2].fill(0);
    whole[3].fill(0);
    check(routed.process({wholeChannels.data(), 4, 104}) &&
          whole[0][7] == .375f && whole[1][7] == 0 &&
          whole[0][87] == 0 && whole[1][87] == .375f,
          "Retargeting did not map the same scene motion to the new outputs");
    check(document.azimuthLayouts()[1].distanceMeters == 2, "Scene distance metadata was lost");
    auto closer = document.serialize();
    const auto distance = closer.find(R"("distanceMeters":2)");
    check(distance != std::string::npos, "Scene distance was not serialized");
    closer.replace(distance, std::string_view(R"("distanceMeters":2)").size(), R"("distanceMeters":1)");
    check(live.replace(graph::Document::parse(closer)), "Live distance edit was rejected");
    for (unsigned c = 0; c < split.size(); ++c)
        split[c].fill(c < 2 ? .25f : 0.f);
    cell_test::audio = true;
    const auto moved = live.process(splitBlock);
    cell_test::audio = false;
    check(moved && split[0][0] > .375f && split[0][0] < .5f &&
          split[0][79] == .5f && split[1][79] == 0,
          "Live source distance did not smooth to the new gain");
    const graph::AutomationPoint retreat{"b", "distanceMeters", 8, 4};
    const auto automated = document.withAutomation({&retreat, 1});
    graph::Player moving(automated, {8000, 104, 4});
    graph::LiveSession movingLive(automated, {8000, 104, 4});
    for (unsigned c = 0; c < split.size(); ++c) {
        split[c].fill(c < 2 ? .25f : 0.f);
        whole[c] = split[c];
    }
    cell_test::audio = true;
    const bool distanceStarted = moving.process({wholeChannels.data(), 4, 104}) &&
        movingLive.process(splitBlock.slice(0, 35));
    cell_test::audio = false;
    check(distanceStarted && movingLive.replace(automated), "Distance trajectory could not be rendered or replaced");
    cell_test::audio = true;
    const bool distanceFinished = movingLive.process(splitBlock.slice(35, 69));
    cell_test::audio = false;
    check(distanceFinished && whole == split && whole[1][7] == .375f &&
          whole[0][87] == .3125f && whole[1][87] == 0,
          "Distance trajectory changed across replacement or interfered with listener yaw");
    auto keyframed = *json::read(scene.serialize()).object();
    keyframed["listenerYaw"] = json::Value(170);
    keyframed["listenerYawCurve"] = json::Value("shortestArc");
    const auto acrossRear = graph::Document::parse(json::write(json::Value(std::move(keyframed))))
        .withListenerYaw(-170, 100);
    graph::Player curved(acrossRear, {8000, 104, 4});
    graph::LiveSession curvedLive(acrossRear, {8000, 104, 4});
    for (unsigned c = 0; c < split.size(); ++c) {
        split[c].fill(c < 2 ? .25f : 0.f);
        whole[c] = split[c];
    }
    cell_test::audio = true;
    const bool curveStarted = curved.process({wholeChannels.data(), 4, 104}) &&
        curvedLive.process(splitBlock.slice(0, 35));
    cell_test::audio = false;
    check(curveStarted && curvedLive.replace(acrossRear), "Keyframed listener replacement failed");
    cell_test::audio = true;
    const bool curveFinished = curvedLive.process(splitBlock.slice(35, 69));
    cell_test::audio = false;
    check(curveFinished && whole == split &&
          whole[0][0] > 0 && whole[2][0] == 0 && whole[3][0] < .375f &&
          whole[3][50] == .375f && whole[0][50] == 0 && whole[2][50] == 0 &&
          whole[0][100] == 0 && whole[2][100] > 0 && whole[2][100] == whole[2][103],
          "Keyframed listener did not cross the rear seam at the exact midpoint");
    const auto paths = graph::Document::parse(R"({
        "schema":"cell.graph.v3","sampleRate":8000,"listenerYaw":170,
        "listenerYawCurve":"shortestArc","speakerLayout":[-90,0,90,180],
        "nodes":[{"id":"source","type":"azimuth","source":0,"azimuth":-170,"azimuthCurve":"shortestArc","distanceMeters":1,"distanceCurve":"linear"}],
        "connections":[{"from":"input","to":"source"},{"from":"source","to":"output"}],
        "intents":[],"automation":[
            {"sample":100,"listenerYaw":-170},
            {"sample":100,"node":"source","parameter":"azimuth","value":170},
            {"sample":100,"node":"source","parameter":"distanceMeters","value":2},
            {"sample":100,"node":"source","parameter":"distanceMeters","value":3}]
    })");
    check(paths.azimuthLayouts().front().interpolatesAzimuth &&
          paths.azimuthLayouts().front().interpolatesDistance, "Source curve metadata was lost");
    graph::Player independent(paths, {8000, 104, 4});
    graph::Player independentSplit(paths, {8000, 104, 4});
    for (unsigned c = 0; c < split.size(); ++c) {
        split[c].fill(c == 0 ? .25f : 0.f);
        whole[c] = split[c];
    }
    cell_test::audio = true;
    const bool sourceRendered = independent.process({wholeChannels.data(), 4, 104}) &&
        independentSplit.process(splitBlock.slice(0, 39)) &&
        independentSplit.process(splitBlock.slice(39, 65));
    cell_test::audio = false;
    check(sourceRendered && whole == split && whole[2][0] > 0 && whole[0][0] == 0 &&
          whole[1][50] == .125f && whole[0][50] == 0 && whole[2][50] == 0 &&
          whole[0][100] > 0 && whole[2][100] == 0 && whole[0][100] == whole[0][103],
          "Source and listener curves did not interpolate independently across the rear seam");
    const auto power = [&](unsigned sample) {
        float total = 0;
        for (const auto& channel : whole)
            total += channel[sample] * channel[sample];
        return total;
    };
    check(std::abs(power(25) - (.25f / 1.5f) * (.25f / 1.5f)) < 1e-6f &&
          std::abs(power(100) - (.25f / 3) * (.25f / 3)) < 1e-6f &&
          power(100) == power(103),
          "Distance keyframes did not interpolate in meters or hold the final target");
    check(cell_test::allocations == 0 && cell_test::deletions == 0,
          "Listener automation allocated or freed on audio");
}

void tremoloModulation() {
    const auto graph = graph::Document::parse(R"({
        "schema":"cell.graph.v3","sampleRate":8000,
        "nodes":[{"id":"pulse","type":"tremolo","rateHz":50,"depth":1}],
        "connections":[{"from":"input","to":"pulse"},{"from":"pulse","to":"output"}],
        "intents":[{"id":"motion","name":"Motion","bindings":[
            {"node":"pulse","parameter":"depth","atZero":0,"atOne":1}]}],
        "automation":[{"sample":100,"intent":"motion","value":0}]
    })");
    const auto controls = graph.controls();
    check(controls.size() == 1 && controls.front().key == "depth" && graph.latency() == 0,
          "Tremolo reported incorrect control or latency");
    graph::Player player(graph, {8000, 200, 2});
    graph::LiveSession live(graph, {8000, 200, 2});
    std::array<std::array<float, 200>, 2> a{}, b{};
    a[0].fill(1);
    a[1].fill(-.5f);
    b = a;
    float* channels[]{a[0].data(), a[1].data()};
    float* alternate[]{b[0].data(), b[1].data()};
    const AudioBlock<float> split{alternate, 2, 200};
    cell_test::audio = true;
    const bool started = player.process({channels, 2, 200}) && live.process(split.slice(0, 113));
    cell_test::audio = false;
    check(started && live.replace(graph), "Could not replace tremolo while playing");
    cell_test::audio = true;
    const bool finished = live.process(split.slice(113, 87));
    cell_test::audio = false;
    check(finished && a == b, "Tremolo phase or depth reset across block boundaries or live replacement");
    check(a[0][0] == 1 && a[0][40] == .5f && a[0][80] == 0 && a[0][190] == 1 &&
          a[1][40] == -.25f && a[1][190] == -.5f,
          "Tremolo cycle or depth did not match its graph settings");
    check(cell_test::allocations == 0 && cell_test::deletions == 0, "Tremolo allocated or freed on audio");
}

void sharedModulation() {
    constexpr auto json = R"({
        "schema":"cell.graph.v4","sampleRate":48000,
        "nodes":[{"id":"g","type":"gain","gainDb":-6},
                 {"id":"t","type":"tremolo","rateHz":100,"depth":0.5}],
        "connections":[{"from":"input","to":"g"},{"from":"g","to":"t"},{"from":"t","to":"output"}],
        "automation":[{"sample":240,"node":"g","parameter":"gainDb","value":0}],
        "intents":[],"lfos":[{"id":"l","rateHz":100}],
        "modulation":[{"source":"l","node":"g","parameter":"gainDb","depth":6},
                      {"source":"l","node":"t","parameter":"depth","depth":-0.25}]
    })";
    const auto graph = graph::Document::parse(json);
    const auto saved = graph::Document::parse(graph.serialize());
    check(graph.serialize() == saved.serialize() && saved.latency() == 0,
          "Modulation changed graph serialization or latency");
    for (const auto& [from, to] : std::array<std::pair<std::string_view, std::string_view>, 4>{{
             {R"("source":"l")", R"("source":"missing")"},
             {R"("parameter":"depth")", R"("parameter":"brightness")"},
             {R"("depth":-0.25)", R"("depth":-1.01)"},
             {R"("rateHz":100}])", R"("rateHz":0}])"}}}) {
        std::string invalid(json);
        invalid.replace(invalid.find(from), from.size(), to);
        bool rejected = false;
        try { (void)graph::Document::parse(invalid); }
        catch (const std::invalid_argument&) { rejected = true; }
        check(rejected, "Invalid modulation was accepted");
    }

    constexpr ProcessSpec spec{48000, 480, 1};
    graph::Player offline(saved, spec);
    graph::LiveSession live(graph, spec, 8);
    std::array<float, 480> a, b;
    a.fill(1.f);
    b.fill(1.f);
    float* offlineChannels[]{a.data()};
    float* liveChannels[]{b.data()};
    const AudioBlock<float> liveBlock{liveChannels, 1, 480};

    cell_test::allocations = cell_test::deletions = 0;
    cell_test::audio = true;
    const bool started = offline.process({offlineChannels, 1, 480}) &&
                         live.process(liveBlock.slice(0, 173));
    cell_test::audio = false;
    check(started && live.replace(saved), "Could not replace modulated graph");
    cell_test::audio = true;
    const bool finished = live.process(liveBlock.slice(173, 92)) &&
                          live.process(liveBlock.slice(265, 215));
    cell_test::audio = false;
    check(finished && a == b, "Modulation changed across live replacement or block sizes");
    check(cell_test::allocations == 0 && cell_test::deletions == 0, "Modulation allocated on audio");
    check(std::abs(a[0] - dbToGain(-6)) < 1e-6f &&
          std::abs(a[120] - .875f) < 1e-6f &&
          std::abs(a[240] - .5f) < 1e-6f &&
          std::abs(a[360] - .625f * dbToGain(-6)) < 1e-6f,
          "Shared LFO did not modulate both targets or respect base automation");

    const std::array edits{graph::ModulationDepthEdit{"l", "g", "gainDb", 72},
                           graph::ModulationDepthEdit{"l", "t", "depth", -.5}};
    const auto batch = graph.withModulationDepths(edits);
    const auto editedJson = cell::json::read(batch.serialize());
    const auto& routes = *editedJson["modulation"].array();
    check(*routes[0]["depth"].number() == 72 && *routes[1]["depth"].number() == -.5 &&
          graph.serialize() == saved.serialize(), "Modulation batch did not preserve the original graph");

    const auto gainEdit = std::span(edits).first(1);
    const auto edited = graph.withModulationDepths(gainEdit);
    graph::Player reference(edited, spec);
    graph::LiveSession changing(graph, spec, 8);
    const graph::ModulationDepthEdit unchanged{"l", "g", "gainDb", 6};
    check(!changing.setModulationDepths({&unchanged, 1}) && !changing.undo(),
          "An unchanged depth created live history");
    for (const auto invalid : {
             graph::ModulationDepthEdit{"l", "t", "depth", -2},
             graph::ModulationDepthEdit{"l", "g", "gainDb", 12},
             graph::ModulationDepthEdit{"l", "t", "gainDb", 0}}) {
        const std::array invalidBatch{edits[0], invalid};
        bool rejected = false;
        try { (void)changing.setModulationDepths(invalidBatch); }
        catch (const std::invalid_argument&) { rejected = true; }
        check(rejected && !changing.updatePending() && !changing.undo() &&
              changing.document().serialize() == graph.serialize(), "Invalid modulation batch changed the session");
    }
    std::array<float, 480> editedOutput;
    editedOutput.fill(1.f);
    b.fill(1.f);
    float* editedChannels[]{editedOutput.data()};
    check(reference.process({editedChannels, 1, 480}) &&
          changing.process(liveBlock.slice(0, 80)) && changing.setModulationDepths(gainEdit) &&
          changing.process(liveBlock.slice(80, 400)), "Live modulation depth edit failed");
    check(std::abs(b[80] - std::lerp(a[80], editedOutput[80], .125f)) < 1e-6f &&
          std::abs(b[120] - .875f * dbToGain(12)) < 1e-6f &&
          std::abs(b[360] - .625f * dbToGain(-60)) < 1e-6f,
          "Modulation edit faded incorrectly, lost phase, or bypassed parameter limits");
    check(changing.undo() && changing.document().serialize() == graph.serialize() &&
          changing.updatePending(), "Undo did not restore the previous modulation depth");
}

void panModulation() {
    constexpr auto json = R"({
        "schema":"cell.graph.v4","sampleRate":48000,
        "nodes":[{"id":"p","type":"pan","source":1,"pan":0}],
        "connections":[{"from":"input","to":"p"},{"from":"p","to":"output"}],
        "automation":[],"intents":[],
        "lfos":[{"id":"a","rateHz":100},{"id":"b","rateHz":100}],
        "modulation":[{"source":"a","node":"p","parameter":"pan","depth":1.5},
                      {"source":"b","node":"p","parameter":"pan","depth":-0.25}]
    })";
    const auto graph = graph::Document::parse(json);
    check(graph.requiredChannels() == 2 && graph.latency() == 0, "Modulated pan changed its layout");
    graph::Player player(graph, {48000, 480, 2});
    std::array<float, 480> left{}, right;
    right.fill(.5f);
    float* channels[]{left.data(), right.data()};
    const AudioBlock<float> block{channels, 2, 480};
    check(player.process(block.slice(0, 89)) && player.process(block.slice(89, 391)),
          "Could not render modulated pan");
    check(left[120] == 0 && right[120] == .5f && left[360] == .5f && right[360] == 0,
          "Modulated pan did not clamp to both channel extremes");
    check(std::abs(left[40] - .5f * std::sqrt(.1875f)) < 1e-6f &&
          std::abs(right[40] - .5f * std::sqrt(.8125f)) < 1e-6f,
          "Modulated pan did not sum both LFOs or select the right input");
    for (unsigned i = 0; i < left.size(); ++i)
        check(std::abs(left[i] * left[i] + right[i] * right[i] - .25f) < 1e-6f,
              "Modulated pan changed total channel power");
}

void lfoWaveforms() {
    constexpr auto json = R"({
        "schema":"cell.graph.v4","sampleRate":51200,
        "nodes":[{"id":"g","type":"gain","gainDb":0}],
        "connections":[{"from":"input","to":"g"},{"from":"g","to":"output"}],
        "automation":[],"intents":[],
        "lfos":[{"id":"l","rateHz":100,"waveform":"triangle","phase":0.25}],
        "modulation":[{"source":"l","node":"g","parameter":"gainDb","depth":6}]
    })";
    const auto parseWith = [=](std::string_view from, std::string_view to) {
        std::string edited(json);
        edited.replace(edited.find(from), from.size(), to);
        return graph::Document::parse(edited);
    };
    const auto triangle = graph::Document::parse(json);
    const auto square = parseWith("triangle", "square");
    for (const auto& [from, to] : std::array<std::pair<std::string_view, std::string_view>, 3>{{
             {"triangle", "unknown"}, {"0.25", "1.01"}, {"0.25", "null"}}}) {
        bool rejected = false;
        try { (void)parseWith(from, to); }
        catch (const std::invalid_argument&) { rejected = true; }
        check(rejected, "Invalid LFO waveform or phase was accepted");
    }

    constexpr ProcessSpec spec{51200, 512, 1};
    graph::Player triPlayer(triangle, spec), squarePlayer(square, spec);
    graph::LiveSession live(triangle, spec, 8);
    std::array<float, 512> triOutput, squareOutput, liveOutput;
    triOutput.fill(1.f);
    squareOutput.fill(1.f);
    liveOutput.fill(1.f);
    float* triChannels[]{triOutput.data()};
    float* squareChannels[]{squareOutput.data()};
    float* liveChannels[]{liveOutput.data()};
    const AudioBlock<float> liveBlock{liveChannels, 1, 512};
    check(triPlayer.process({triChannels, 1, 512}) && squarePlayer.process({squareChannels, 1, 512}),
          "Could not render the new LFO waveforms");
    check(triOutput[0] == dbToGain(6) && triOutput[64] == dbToGain(3) &&
          triOutput[128] == 1.f && triOutput[256] == dbToGain(-6), "Triangle LFO shape or phase is wrong");
    check(squareOutput[127] == dbToGain(6) && squareOutput[128] == dbToGain(-6) &&
          squareOutput[383] == dbToGain(-6) && squareOutput[384] == dbToGain(6),
          "Square LFO edges were not aligned to the sample clock");
    check(live.process(liveBlock.slice(0, 120)) && live.replace(square) &&
          live.process(liveBlock.slice(120, 13)) && live.process(liveBlock.slice(133, 379)),
          "Live waveform replacement failed");
    check(liveOutput[120] == std::lerp(triOutput[120], squareOutput[120], .125f) &&
          std::equal(liveOutput.begin() + 128, liveOutput.end(), squareOutput.begin() + 128),
          "Live waveform edit lost its crossfade or sample phase");
}

void azimuthModulation() {
    constexpr auto text = R"({
        "schema":"cell.graph.v4","sampleRate":51200,"listenerYaw":90,
        "speakerLayout":[0,90,180,-90],
        "nodes":[{"id":"s","type":"azimuth","source":0,"azimuth":0,"azimuthCurve":"shortestArc"}],
        "connections":[{"from":"input","to":"s"},{"from":"s","to":"output"}],
        "automation":[{"sample":256,"node":"s","parameter":"azimuth","value":90}],
        "intents":[],
        "lfos":[{"id":"a","rateHz":100,"waveform":"square"},
                {"id":"b","rateHz":100,"waveform":"square"},
                {"id":"c","rateHz":100,"waveform":"square"}],
        "modulation":[{"source":"a","node":"s","parameter":"azimuth","depth":360},
                      {"source":"b","node":"s","parameter":"azimuth","depth":360},
                      {"source":"c","node":"s","parameter":"azimuth","depth":90}]
    })";
    const auto graph = graph::Document::parse(text);
    constexpr ProcessSpec spec{51200, 512, 4};
    graph::Player player(graph, spec);
    graph::LiveSession live(graph, spec, 8);
    std::array<std::array<float, 512>, 4> a{}, b{};
    a[0].fill(.5f);
    b[0].fill(.5f);
    float* channels[]{a[0].data(), a[1].data(), a[2].data(), a[3].data()};
    float* liveChannels[]{b[0].data(), b[1].data(), b[2].data(), b[3].data()};
    const AudioBlock<float> liveBlock{liveChannels, 4, 512};

    cell_test::allocations = cell_test::deletions = 0;
    cell_test::audio = true;
    const bool started = player.process({channels, 4, 512}) &&
                         live.process(liveBlock.slice(0, 173));
    cell_test::audio = false;
    check(started && live.replace(graph), "Could not replace the spatially modulated graph");
    cell_test::audio = true;
    const bool finished = live.process(liveBlock.slice(173, 339));
    cell_test::audio = false;
    check(finished && a == b && cell_test::allocations == 0 && cell_test::deletions == 0,
          "Azimuth modulation changed across live blocks or allocated on audio");
    check(a[0][0] == .5f && a[1][0] == 0 && a[2][0] == 0 && a[3][0] == 0 &&
          std::abs(a[0][128] - .5f * std::sqrt(.5f)) < 1e-6f &&
          std::abs(a[1][128] - a[0][128]) < 1e-6f &&
          a[3][256] == .5f && a[0][256] == 0 && a[1][256] == 0 && a[2][256] == 0,
          "Azimuth modulation did not wrap both directions or combine with keyframes and yaw");
}

void driveModulation() {
    constexpr auto text = R"({
        "schema":"cell.graph.v4","sampleRate":51200,
        "nodes":[{"id":"s","type":"saturator","driveDb":6,"oversampling":4}],
        "connections":[{"from":"input","to":"s"},{"from":"s","to":"output"}],
        "automation":[{"sample":128,"node":"s","parameter":"driveDb","value":18}],
        "intents":[],
        "lfos":[{"id":"l","rateHz":100,"waveform":"square"}],
        "modulation":[{"source":"l","node":"s","parameter":"driveDb","depth":24}]
    })";
    constexpr ProcessSpec spec{51200, 512, 1};
    for (const auto factor : {1u, 4u}) {
        std::string graphText(text);
        if (factor == 1)
            graphText.replace(graphText.find(R"("oversampling":4)"), std::string_view(R"("oversampling":4)").size(),
                              R"("oversampling":1)");
        const auto graph = graph::Document::parse(graphText);
        const auto expectedLatency = factor == 4 ? dsp::Oversampler<2>::latency : 0;
        check(graph.latency() == expectedLatency && graph.tail() == expectedLatency,
              "Drive modulation changed saturator latency or tail");
        graph::Player player(graph, spec);
        graph::LiveSession live(graph, spec, 8);
        std::array<float, 512> a, b;
        a.fill(.0625f);
        b = a;
        float* channels[]{a.data()};
        float* liveChannels[]{b.data()};
        const AudioBlock<float> block{liveChannels, 1, 512};

        cell_test::allocations = cell_test::deletions = 0;
        cell_test::audio = true;
        const bool started = player.process({channels, 1, 512}) && live.process(block.slice(0, 191));
        cell_test::audio = false;
        check(started && live.replace(graph), "Could not replace the modulated saturator");
        cell_test::audio = true;
        const bool finished = live.process(block.slice(191, 321));
        cell_test::audio = false;
        check(finished && a == b && cell_test::allocations == 0 && cell_test::deletions == 0,
              "Drive modulation changed across live blocks or allocated on audio");

        dsp::Oversampler<2> reference;
        for (unsigned frame = 0; frame < a.size(); ++frame) {
            const auto drive = double(dbToGain(frame < 128 ? 30.f : frame < 256 ? 36.f : 0.f));
            const auto transfer = [drive](double input) { return std::tanh(input * drive); };
            const auto expected = factor == 4 ? reference.process(.0625, transfer) : transfer(.0625);
            check(std::abs(a[frame] - float(expected)) < 1e-6f,
                  "Drive modulation missed automation, clamp or oversampled frame alignment");
        }
    }
}

void tempoSyncedModulation() {
    constexpr auto text = R"({
        "schema":"cell.graph.v4","sampleRate":51200,
        "nodes":[{"id":"g","type":"gain","gainDb":0}],
        "connections":[{"from":"input","to":"g"},{"from":"g","to":"output"}],
        "automation":[],"intents":[],
        "tempoMap":[{"sample":0,"bpm":187.5},{"sample":256,"bpm":375}],
        "lfos":[{"id":"l","beatsPerCycle":0.125,"waveform":"triangle"}],
        "modulation":[{"source":"l","node":"g","parameter":"gainDb","depth":6}]
    })";
    for (const auto& [from, to] : std::array<std::pair<std::string_view, std::string_view>, 5>{{
             {R"("sample":0)", R"("sample":1)"}, {R"("sample":256)", R"("sample":0)"},
             {R"("bpm":375)", R"("bpm":0)"}, {R"("beatsPerCycle":0.125)", R"("beatsPerCycle":0.125,"rateHz":1)"},
             {R"("tempoMap":[{"sample":0,"bpm":187.5},{"sample":256,"bpm":375}],)", ""}}}) {
        std::string invalid(text);
        invalid.replace(invalid.find(from), from.size(), to);
        bool rejected = false;
        try { (void)graph::Document::parse(invalid); }
        catch (const std::invalid_argument&) { rejected = true; }
        check(rejected, "Invalid tempo map or beat-synced LFO was accepted");
    }
    const auto graph = graph::Document::parse(text);
    const auto saved = graph::Document::parse(graph.serialize());
    constexpr ProcessSpec spec{51200, 1024, 1};
    graph::Player offline(saved, spec);
    graph::LiveSession live(graph, spec, 8);
    std::array<float, 1024> a, b;
    a.fill(1.f);
    b = a;
    float* offlineChannels[]{a.data()};
    float* liveChannels[]{b.data()};
    const AudioBlock<float> block{liveChannels, 1, 1024};
    cell_test::allocations = cell_test::deletions = 0;
    cell_test::audio = true;
    const bool started = offline.process({offlineChannels, 1, 1024}) && live.process(block.slice(0, 271));
    cell_test::audio = false;
    check(started && live.replace(saved), "Could not replace beat-synced graph after a tempo change");
    cell_test::audio = true;
    const bool finished = live.process(block.slice(271, 753));
    cell_test::audio = false;
    check(finished && a == b && cell_test::allocations == 0 && cell_test::deletions == 0,
          "Beat-synced modulation changed across live blocks or allocated on audio");
    check(a[0] == 1.f && a[256] == dbToGain(3) && a[384] == dbToGain(6) &&
          a[640] == 1.f && a[896] == dbToGain(-6), "LFO phase jumped at the tempo change");
    check(a[255] < a[256] && a[256] < a[257], "Tempo change did not continue the triangle ramp");
}

void hostClockModulation() {
    constexpr auto text = R"({
        "schema":"cell.graph.v4","sampleRate":51200,
        "nodes":[{"id":"g","type":"gain","gainDb":0}],
        "connections":[{"from":"input","to":"g"},{"from":"g","to":"output"}],
        "automation":[{"sample":32,"node":"g","parameter":"gainDb","value":6}],"intents":[],
        "tempoMap":[{"sample":0,"bpm":187.5}],
        "lfos":[{"id":"l","beatsPerCycle":0.125,"waveform":"triangle","clock":"host"}],
        "modulation":[{"source":"l","node":"g","parameter":"gainDb","depth":6}]
    })";
    const auto document = graph::Document::parse(text);
    constexpr ProcessSpec spec{51200, 64, 2};
    graph::Player player(document, spec), fallback(document, spec);
    graph::LiveSession live(document, spec, 8);
    Engine engine(example::graphDescriptor, std::make_unique<example::GraphProcessor>(document));
    engine.prepare(spec.sampleRate, spec.maxFrames, spec.channels);
    std::array<std::array<float, 64>, 2> rendered, audition, expected;
    const auto fill = [](auto& samples) { for (auto& channel : samples) channel.fill(1.f); };
    float* directChannels[]{rendered[0].data(), rendered[1].data()};
    float* liveChannels[]{audition[0].data(), audition[1].data()};
    float* expectedChannels[]{expected[0].data(), expected[1].data()};
    const AudioBlock<float> direct{directChannels, 2, 64}, liveBlock{liveChannels, 2, 64};
    Transport host;
    host.beatPositionValid = true;
    host.playing = true;
    host.tempo = 375;
    host.beatPosition = .03125;
    fill(rendered);
    fill(audition);
    fill(expected);
    check(player.process(direct, host) && live.process(liveBlock, host), "Host clock render failed");
    engine.process({expectedChannels, 2, 64}, {}, host);
    check(rendered == audition && rendered == expected && rendered[0][0] == dbToGain(6) &&
          rendered[0][32] == dbToGain(11.25f),
          "Host clock did not reach the graph or reset at a graph automation split");

    host.beatPosition = -.03125;
    host.playing = false;
    fill(rendered);
    check(player.process(direct, host) && std::all_of(rendered[0].begin(), rendered[0].end(),
          [](float value) { return value == 1; }), "Stopped or negative host position was not honored");

    player.reset();
    host.beatPositionValid = false;
    fill(rendered);
    fill(expected);
    check(player.process(direct, host) && fallback.process({expectedChannels, 2, 64}) &&
          rendered == expected && rendered[0][0] == 1, "Missing host timeline did not use saved tempo");
}

} // namespace

int main() {
    try {
        run();
        liveEdits();
        parallelGraph();
        liveParallelRouting();
        latencyCompensation();
        liveCompensation();
        automationControls();
        musicalIntents();
        saturation();
        multichannelRouting();
        stereoPosition();
        surroundPosition();
        listenerOrientation();
        tremoloModulation();
        sharedModulation();
        panModulation();
        lfoWaveforms();
        azimuthModulation();
        driveModulation();
        tempoSyncedModulation();
        hostClockModulation();
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
