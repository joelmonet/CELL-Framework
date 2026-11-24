// Original Cell Framework implementation; see LICENSE.
#include <cell/portable_graph.hpp>
#include <cell/json.hpp>
#include <cell/realtime.hpp>
#include <initializer_list>
#include <limits>
#include <map>
#include <numbers>
#include <set>

namespace cell::graph {
namespace {

using json::Value;

void require(bool condition, const char* message) {
    if (!condition)
        throw std::invalid_argument(message);
}

void fields(const Value& value, std::initializer_list<std::string_view> names,
            std::initializer_list<std::string_view> optional = {}) {
    const auto* object = value.object();
    require(object, "Expected a graph object");
    const auto present = std::count_if(optional.begin(), optional.end(),
                                       [&](auto key) { return object->contains(key); });
    require(object->size() == names.size() + present, "Unexpected graph fields");
    for (const auto name : names)
        require(object->contains(name), "Missing graph field");
}

const Value::Array& array(const Value& value, std::size_t limit) {
    const auto* result = value.array();
    require(result && result->size() <= limit, "Invalid graph array or size");
    return *result;
}

std::uint64_t integer(const Value& value) {
    if (const auto* n = value.unsignedInteger())
        return *n;
    const auto* n = value.signedInteger();
    require(n && *n >= 0, "Expected a nonnegative graph integer");
    return static_cast<std::uint64_t>(*n);
}

double number(const Value& value, double minimum, double maximum) {
    const auto* n = value.number();
    require(n && *n >= minimum && *n <= maximum, "Graph parameter out of range");
    return *n;
}


constexpr std::size_t maxAutomation = 32768;
constexpr double maxDistanceMeters = 100000;
constexpr std::string_view distanceKey = "distanceMeters";

float distanceGain(double meters) noexcept {
    return 1.f / std::max(1.f, static_cast<float>(meters));
}
constexpr std::uint8_t silentChannel = 255;

float wrapAzimuth(float angle) noexcept {
    if (angle < 0)
        angle += 360;
    return angle >= 360 ? angle - 360 : angle;
}

void targetAzimuth(dsp::SmoothedValue& smoother, float& target, float heading) noexcept {
    if (heading == target)
        return;
    const auto current = wrapAzimuth(smoother.current());
    smoother.reset(current);
    smoother.target(current + std::remainder(heading - current, 360.0f));
    target = heading;
}

Document appendAutomation(const Document& document, Value::Array additions) {
    auto fields = *json::read(document.serialize()).object();
    auto automation = *fields.at("automation").array();
    const auto oldSize = automation.size();
    require(additions.size() <= maxAutomation - oldSize, "Too many graph automation points");
    automation.insert(automation.end(), std::make_move_iterator(additions.begin()),
                      std::make_move_iterator(additions.end()));
    const auto earlier = [](const Value& a, const Value& b) {
        return integer(a["sample"]) < integer(b["sample"]);
    };
    const auto insertion = automation.begin() + static_cast<std::ptrdiff_t>(oldSize);
    std::stable_sort(insertion, automation.end(), earlier);
    std::inplace_merge(automation.begin(), insertion, automation.end(), earlier);
    fields.at("automation") = Value(std::move(automation));
    return Document::parse(json::write(Value(std::move(fields))));
}

} // namespace

double ControlMapping::map(double normalized) const noexcept {
    return minimum * std::pow(maximum / minimum, normalized);
}

std::string_view Document::Node::controlKey() const noexcept {
    switch (kind) {
    case Kind::gain: return "gainDb";
    case Kind::lowpass: return "brightness";
    case Kind::delay:
    case Kind::channelMap: return {};
    case Kind::saturator: return "driveDb";
    case Kind::pan: return "pan";
    case Kind::azimuth: return "azimuth";
    case Kind::tremolo: return "depth";
    }
    return {};
}

std::uint32_t Document::Node::latency() const noexcept {
    switch (oversampling) {
    case 2: return dsp::Oversampler<1>::latency;
    case 4: return dsp::Oversampler<2>::latency;
    case 8: return dsp::Oversampler<3>::latency;
    case 16: return dsp::Oversampler<4>::latency;
    default: return delayFrames;
    }
}

Document Document::parse(std::string_view text) {
    const auto root = json::read(text);
    const auto schema = root["schema"].text();
    const bool hasModulation = schema == "cell.graph.v4";
    const bool hasIntents = hasModulation || schema == "cell.graph.v3";
    const bool parallel = hasIntents || schema == "cell.graph.v2";
    require(parallel || schema == "cell.graph.v1", "Unsupported graph schema");
    if (hasModulation)
        fields(root, {"schema", "sampleRate", "nodes", "connections", "automation", "intents", "lfos", "modulation"},
               {"listenerYaw", "listenerYawCurve", "speakerLayout", "tempoMap"});
    else if (hasIntents)
        fields(root, {"schema", "sampleRate", "nodes", "connections", "automation", "intents"},
               {"listenerYaw", "listenerYawCurve", "speakerLayout"});
    else
        fields(root, {"schema", "sampleRate", "nodes", "connections", "automation"});
    const bool hasListener = root.object()->contains("listenerYaw");
    const bool hasLayout = root.object()->contains("speakerLayout");
    bool usesLayout = false;
    const auto rate = integer(root["sampleRate"]);
    require(rate >= 8000 && rate <= 384000, "Unsupported graph sample rate");

    const auto& nodes = array(root["nodes"], 64);
    const auto& connections = array(root["connections"], parallel ? 256 : 65);
    require(!nodes.empty(), "Graph needs at least one processing node");
    require(parallel || connections.size() == nodes.size() + 1, "Expected one input-to-output chain");

    Document document;
    document.sampleRate_ = static_cast<unsigned>(rate);
    if (hasListener)
        document.listenerYaw_ = static_cast<float>(number(root["listenerYaw"], -180, 180));
    if (root.object()->contains("listenerYawCurve")) {
        require(hasListener && root["listenerYawCurve"].text() == "shortestArc",
                "Listener yaw curve requires listenerYaw and shortestArc");
        document.interpolateListenerYaw_ = true;
    }
    const auto setWidth = [&](unsigned channels) {
        require(document.requiredChannels_ == 0 || document.requiredChannels_ == channels,
                "Graph nodes must agree on a channel count");
        document.requiredChannels_ = channels;
    };
    std::map<std::string_view, Node> byId;
    for (const auto& node : nodes) {
        const auto id = node["id"].text();
        require(!id.empty() && id.size() <= 64 && id != "input" && id != "output",
                "Invalid graph node ID");
        const auto type = node["type"].text();
        Node definition{};
        if (type == "gain") {
            fields(node, {"id", "type", "gainDb"});
            definition = {std::string(id), Kind::gain, number(node["gainDb"], -60, 12), -60, 12};
        } else if (parallel && type == "channelMap") {
            fields(node, {"id", "type", "sources"});
            const auto& sources = array(node["sources"], 64);
            require(!sources.empty(), "Channel map needs at least one output");
            setWidth(static_cast<unsigned>(sources.size()));
            definition.id = id;
            definition.kind = Kind::channelMap;
            for (const auto& source : sources) {
                const auto index = source.isNull() ? std::uint64_t(silentChannel) : integer(source);
                require(source.isNull() || index < sources.size(), "Channel map source index out of range");
                definition.channelMap.push_back(static_cast<std::uint8_t>(index));
            }
        } else if (parallel && type == "azimuth") {
            if (hasIntents)
                fields(node, {"id", "type", "source", "azimuth"}, {"speakers", "distanceMeters", "azimuthCurve", "distanceCurve"});
            else
                fields(node, {"id", "type", "source", "azimuth", "speakers"});
            const bool inheritsLayout = !node.object()->contains("speakers");
            require(!inheritsLayout || hasLayout, "Azimuth needs speakers or a shared speaker layout");
            usesLayout |= inheritsLayout;
            const auto& speakers = array(inheritsLayout ? root["speakerLayout"] : node["speakers"], 64);
            const auto source = integer(node["source"]);
            require(source < speakers.size(), "Azimuth source channel is outside the speaker layout");
            setWidth(static_cast<unsigned>(speakers.size()));
            definition = {std::string(id), Kind::azimuth, number(node["azimuth"], -180, 180), -180, 180};
            definition.panSource = static_cast<unsigned>(source);
            if (node.object()->contains("azimuthCurve")) {
                require(node["azimuthCurve"].text() == "shortestArc", "Unsupported source azimuth curve");
                definition.interpolatesAzimuth = true;
            }
            definition.distanceControl = node.object()->contains(distanceKey);
            if (definition.distanceControl)
                definition.distanceMeters = static_cast<float>(number(node[distanceKey], 0, maxDistanceMeters));
            if (node.object()->contains("distanceCurve")) {
                require(definition.distanceControl && node["distanceCurve"].text() == "linear",
                        "Distance curve requires distanceMeters and linear interpolation");
                definition.interpolatesDistance = true;
            }
            for (unsigned c = 0; c < speakers.size(); ++c) {
                if (!speakers[c].isNull())
                    definition.speakers.push_back({wrapAzimuth(static_cast<float>(number(speakers[c], -180, 180))), c});
            }
            auto& ordered = definition.speakers;
            require(ordered.size() >= 3, "Azimuth needs at least three directional speakers");
            std::sort(ordered.begin(), ordered.end(), [](const Speaker& a, const Speaker& b) { return a.angle < b.angle; });
            for (std::size_t i = 0; i < ordered.size(); ++i) {
                const auto next = i + 1 < ordered.size() ? ordered[i + 1].angle : ordered.front().angle + 360;
                const auto gap = next - ordered[i].angle;
                require(gap > 0 && gap <= 180, "Azimuth speakers must be distinct with gaps no greater than 180 degrees");
            }
        } else if (parallel && type == "pan") {
            fields(node, {"id", "type", "source", "pan"});
            const auto source = integer(node["source"]);
            require(source < 2, "Stereo pan source must be channel 0 or 1");
            setWidth(2);
            definition = {std::string(id), Kind::pan, number(node["pan"], -1, 1), -1, 1};
            definition.panSource = static_cast<unsigned>(source);
        } else if (parallel && type == "tremolo") {
            fields(node, {"id", "type", "rateHz", "depth"});
            definition = {std::string(id), Kind::tremolo, number(node["depth"], 0, 1), 0, 1};
            definition.rateHz = number(node["rateHz"], .01, 100);
        } else if (parallel && type == "saturator") {
            fields(node, {"id", "type", "driveDb", "oversampling"});
            const auto factor = integer(node["oversampling"]);
            require(factor == 1 || factor == 2 || factor == 4 || factor == 8 || factor == 16,
                    "Saturator oversampling must be 1, 2, 4, 8 or 16");
            definition = {std::string(id), Kind::saturator, number(node["driveDb"], 0, 36),
                          0, 36, 0, static_cast<unsigned>(factor)};
        } else if (parallel && type == "delay") {
            fields(node, {"id", "type", "frames"});
            const auto frames = integer(node["frames"]);
            require(frames > 0 && frames <= 1048576, "Delay node frames must be in [1, 1048576]");
            definition = {std::string(id), Kind::delay, 0, 0, 0, static_cast<std::uint32_t>(frames)};
        } else {
            require(type == "lowpass", "Unsupported graph node type");
            fields(node, {"id", "type", "brightness", "minHz", "maxHz"});
            const auto minimum = number(node["minHz"], 20, rate * .49);
            const auto maximum = number(node["maxHz"], minimum, rate * .49);
            require(maximum > minimum, "Brightness range must increase");
            definition = {std::string(id), Kind::lowpass, number(node["brightness"], 0, 1), minimum, maximum};
        }
        require(byId.emplace(id, definition).second, "Duplicate graph node ID");
    }

    std::vector<std::pair<std::string_view, std::string_view>> edges;
    for (const auto& connection : connections) {
        fields(connection, {"from", "to"});
        const auto from = connection["from"].text();
        const auto to = connection["to"].text();
        require((from == "input" || byId.contains(from)) &&
                (to == "output" || byId.contains(to)), "Unknown or invalid audio port");
        edges.emplace_back(from, to);
    }

    std::map<std::string_view, std::size_t> order;
    if (!parallel) {
        std::map<std::string_view, std::string_view> next;
        for (const auto& [from, to] : edges)
            require(next.emplace(from, to).second, "Multiple outgoing audio connections");
        require(next.contains("input"), "Missing graph input connection");
        auto endpoint = next.at("input");
        while (endpoint != "output") {
            require(order.emplace(endpoint, document.nodes_.size()).second, "Graph contains a cycle");
            document.nodes_.push_back(byId.at(endpoint));
            const auto connection = next.find(endpoint);
            require(connection != next.end(), "Unconnected graph node");
            endpoint = connection->second;
        }
        require(order.size() == nodes.size(), "Graph has nodes outside the audio chain");
    } else {
        std::map<std::string_view, std::set<std::string_view>> sources;
        for (const auto& [from, to] : edges)
            require(sources[to].insert(from).second, "Duplicate graph connection");
        require(sources.contains("output"), "Missing graph output connection");
        for (const auto& entry : byId)
            require(sources.contains(entry.first), "Graph node has no audio input");

        // Select the first ready ID in lexical order. This fixes processing
        // and summing order independently of the declaration order.
        while (document.nodes_.size() < byId.size()) {
            const auto ready = std::find_if(byId.begin(), byId.end(), [&](const auto& entry) {
                const auto& inputs = sources.at(entry.first);
                return !order.contains(entry.first) && std::all_of(inputs.begin(), inputs.end(),
                    [&](std::string_view id) { return id == "input" || order.contains(id); });
            });
            require(ready != byId.end(), "Graph contains a cycle");
            order.emplace(ready->first, document.nodes_.size());
            document.nodes_.push_back(ready->second);
        }
        document.routing_.resize(nodes.size() + 1);
        for (const auto& [target, inputs] : sources) {
            auto& routes = document.routing_[target == "output" ? nodes.size() : order.at(target)];
            for (const auto input : inputs)
                routes.push_back({input == "input" ? 0 : order.at(input) + 1, 0});
            std::sort(routes.begin(), routes.end(), [](const Route& a, const Route& b) {
                return a.source < b.source;
            });
        }

        std::vector<bool> reachesOutput(nodes.size() + 1, false);
        reachesOutput.back() = true;
        for (std::size_t i = document.routing_.size(); i-- > 0;) {
            require(reachesOutput[i], "Graph node has no route to output");
            for (const auto route : document.routing_[i])
                if (route.source != 0)
                    reachesOutput[route.source - 1] = true;
        }

        // Host input is arrival 0. Align incoming paths at each destination
        // before adding the destination node's own fixed latency.
        std::vector<std::uint32_t> arrival(nodes.size() + 1), ringing(nodes.size() + 1);
        for (std::size_t i = 0; i < document.routing_.size(); ++i) {
            auto& routes = document.routing_[i];
            std::uint32_t latest{}, tail{};
            for (const auto route : routes) {
                latest = std::max(latest, arrival[route.source]);
                tail = std::max(tail, ringing[route.source]);
            }
            for (auto& route : routes)
                route.delay = latest - arrival[route.source];
            if (i < nodes.size()) {
                const auto& node = document.nodes_[i];
                arrival[i + 1] = latest + node.latency();
                ringing[i + 1] = tail + (node.kind == Kind::saturator ? node.latency() : 0);
            } else {
                document.latency_ = latest;
                document.tail_ = tail;
            }
        }
    }

    const auto control = [&](const Value& fields) {
        const auto target = order.find(fields["node"].text());
        require(target != order.end(), "Unknown automation node");
        const auto& node = document.nodes_[target->second];
        require(!node.controlKey().empty(), "Graph node has no automatable controls");
        const auto key = fields["parameter"].text();
        const bool distance = node.distanceControl && key == distanceKey;
        require(distance || key == node.controlKey(), "Unsupported automation parameter on node");
        return std::pair{target->second, distance};
    };

    if (hasModulation) {
        if (root.object()->contains("tempoMap")) {
            const auto& points = array(root["tempoMap"], 1024);
            require(!points.empty(), "Tempo map must start at sample zero");
            for (const auto& point : points) {
                fields(point, {"sample", "bpm"});
                const auto sample = integer(point["sample"]);
                require(document.tempoMap_.empty() ? sample == 0 : sample > document.tempoMap_.back().sample,
                        "Tempo map must start at zero and advance in sample order");
                document.tempoMap_.push_back({sample, number(point["bpm"], 1, 1000)});
            }
        }
        std::map<std::string_view, std::size_t> lfoIds;
        for (const auto& source : array(root["lfos"], 16)) {
            fields(source, {"id"}, {"rateHz", "beatsPerCycle", "waveform", "phase", "clock"});
            const bool beatSynced = source.object()->contains("beatsPerCycle");
            require(beatSynced != source.object()->contains("rateHz"), "LFO needs exactly one rate mode");
            require(!beatSynced || !document.tempoMap_.empty(), "Beat-synced LFO requires a tempo map");
            const auto id = source["id"].text();
            require(!id.empty() && id.size() <= 64 &&
                    lfoIds.emplace(id, document.lfos_.size()).second, "Invalid or duplicate LFO ID");
            const bool followsHost = source.object()->contains("clock");
            require(!followsHost || (beatSynced && source["clock"].text() == "host"),
                    "Host clock requires a beat-synced LFO");
            auto waveform = LfoWaveform::sine;
            if (source.object()->contains("waveform")) {
                const auto shape = source["waveform"].text();
                if (shape == "triangle")
                    waveform = LfoWaveform::triangle;
                else if (shape == "square")
                    waveform = LfoWaveform::square;
                else
                    require(shape == "sine", "Unsupported LFO waveform");
            }
            const auto phase = source.object()->contains("phase") ? number(source["phase"], 0, 1) : 0;
            const auto rate = beatSynced ? number(source["beatsPerCycle"], .125, 128)
                                         : number(source["rateHz"], .01, 100);
            document.lfos_.push_back({rate, phase == 1 ? 0 : phase, beatSynced, followsHost, waveform});
        }
        std::set<std::pair<std::size_t, std::size_t>> destinations;
        for (const auto& route : array(root["modulation"], 64)) {
            fields(route, {"source", "node", "parameter", "depth"});
            const auto source = lfoIds.find(route["source"].text());
            require(source != lfoIds.end(), "Unknown modulation source");
            const auto target = order.find(route["node"].text());
            require(target != order.end(), "Unknown modulation target node");
            const auto& node = document.nodes_[target->second];
            require((node.kind == Kind::gain || node.kind == Kind::tremolo ||
                     node.kind == Kind::pan || node.kind == Kind::azimuth || node.kind == Kind::saturator) &&
                    route["parameter"].text() == node.controlKey(), "Unsupported modulation target parameter");
            const auto range = node.maximum - node.minimum;
            const auto depth = number(route["depth"], -range, range);
            require(destinations.emplace(source->second, target->second).second, "Duplicate modulation route");
            document.modulation_.push_back({source->second, target->second, depth});
        }
    }

    std::map<std::string_view, std::size_t> intentIds;
    if (hasIntents) {
        for (const auto& value : array(root["intents"], 32)) {
            fields(value, {"id", "name", "bindings"});
            const auto id = value["id"].text();
            const auto name = value["name"].text();
            require(!id.empty() && id.size() <= 64 && !name.empty() && name.size() <= 128,
                    "Invalid intent ID or name");
            require(intentIds.emplace(id, document.intents_.size()).second, "Duplicate graph intent ID");
            Intent intent{std::string(id), std::string(name), {}};
            std::set<std::pair<std::size_t, bool>> targets;
            for (const auto& binding : array(value["bindings"], 64)) {
                const auto* object = binding.object();
                const bool hasInterpolation = object && object->contains("interpolation");
                if (hasInterpolation)
                    fields(binding, {"node", "parameter", "atZero", "atOne", "interpolation"});
                else
                    fields(binding, {"node", "parameter", "atZero", "atOne"});
                const auto [target, distance] = control(binding);
                require(targets.emplace(target, distance).second, "Duplicate binding in graph intent");
                const auto& node = document.nodes_[target];
                auto interpolation = IntentBinding::Interpolation::linear;
                if (hasInterpolation) {
                    require(!distance && node.kind == Kind::azimuth && binding["interpolation"].text() == "shortestArc",
                            "Shortest-arc interpolation is only supported for azimuth");
                    interpolation = IntentBinding::Interpolation::shortestArc;
                }
                const auto minimum = distance ? 0 : node.controlMin();
                const auto maximum = distance ? maxDistanceMeters : node.controlMax();
                intent.bindings.push_back({node.id, std::string(binding["parameter"].text()),
                    number(binding["atZero"], minimum, maximum),
                    number(binding["atOne"], minimum, maximum), interpolation});
            }
            require(!intent.bindings.empty(), "Graph intent has no bindings");
            document.intents_.push_back(std::move(intent));
        }
    }

    for (const auto& event : array(root["automation"], maxAutomation)) {
        const auto sample = integer(event["sample"]);
        require(document.events_.empty() || sample >= document.events_.back().sample,
                "Graph automation must be in sample order");
        if (event.object() && event.object()->contains("listenerYaw")) {
            fields(event, {"sample", "listenerYaw"});
            require(hasListener && document.events_.size() < maxAutomation,
                    "Listener automation requires a listener and space on the automation timeline");
            document.events_.push_back({sample, listenerEvent, number(event["listenerYaw"], -180, 180)});
        } else if (hasIntents && !event["intent"].isNull()) {
            fields(event, {"sample", "intent", "value"});
            const auto id = intentIds.find(event["intent"].text());
            require(id != intentIds.end(), "Unknown graph intent");
            const auto normalized = number(event["value"], 0, 1);
            const auto& bindings = document.intents_[id->second].bindings;
            require(bindings.size() <= maxAutomation - document.events_.size(),
                    "Expanded graph automation exceeds the point limit");
            for (const auto& binding : bindings) {
                auto value = std::lerp(binding.atZero, binding.atOne, normalized);
                if (binding.interpolation == IntentBinding::Interpolation::shortestArc &&
                    normalized > 0 && normalized < 1) {
                    const auto arc = std::remainder(binding.atOne - binding.atZero, 360.0);
                    value = std::remainder(binding.atZero + normalized * arc, 360.0);
                }
                document.events_.push_back({sample, order.at(binding.node), value, binding.parameter == distanceKey});
            }
        } else {
            fields(event, {"sample", "node", "parameter", "value"});
            const auto [target, distance] = control(event);
            const auto& node = document.nodes_[target];
            require(document.events_.size() < maxAutomation, "Expanded graph automation exceeds the point limit");
            document.events_.push_back({sample, target,
                number(event["value"], distance ? 0 : node.controlMin(),
                       distance ? maxDistanceMeters : node.controlMax()), distance});
        }
    }
    require(!hasLayout || usesLayout, "Shared speaker layout needs at least one azimuth source");
    document.json_ = json::write(root);
    return document;
}

std::vector<Control> Document::controls() const {
    std::vector<Control> result;
    for (const auto& node : nodes_) {
        if (node.controlKey().empty())
            continue;
        const bool filter = node.kind == Kind::lowpass;
        const bool azimuth = node.kind == Kind::azimuth;
        const bool position = node.kind == Kind::pan || azimuth;
        const bool tremolo = node.kind == Kind::tremolo;
        const auto meaning = filter ? "brightness" : position ? "position" : tremolo ? "depth" :
                             node.kind == Kind::saturator ? "drive" : "level";
        result.push_back({node.id, node.controlKey(), meaning, azimuth ? "degrees" : (filter || position || tremolo) ? "normalized" : "dB",
            node.controlMin(), node.controlMax(), node.initial,
            filter ? std::optional{node.cutoffMapping()} : std::nullopt});
        if (node.distanceControl)
            result.push_back({node.id, distanceKey, "distance", "meters",
                              0, maxDistanceMeters, node.distanceMeters, std::nullopt});
    }
    return result;
}

std::vector<AzimuthLayout> Document::azimuthLayouts() const {
    std::vector<AzimuthLayout> result;
    for (const auto& node : nodes_) {
        if (node.kind != Kind::azimuth)
            continue;
        AzimuthLayout layout{node.id, node.panSource, std::vector<std::optional<float>>(requiredChannels_), node.distanceMeters, node.interpolatesAzimuth, node.interpolatesDistance};
        for (const auto& speaker : node.speakers)
            layout.speakerDegrees[speaker.channel] = speaker.angle > 180 ? speaker.angle - 360 : speaker.angle;
        result.push_back(std::move(layout));
    }
    return result;
}

Document Document::withAutomation(std::span<const AutomationPoint> points) const {
    if (points.empty())
        return *this;
    require(points.size() <= maxAutomation - events_.size(), "Too many graph automation points");
    Value::Array automation;
    automation.reserve(points.size());
    for (const auto& point : points) {
        require(std::isfinite(point.value), "Graph automation value must be finite");
        automation.emplace_back(Value::Object{
            {"sample", Value(point.sample)}, {"node", Value(std::string(point.node))},
            {"parameter", Value(std::string(point.parameter))}, {"value", Value(point.value)}});
    }
    return appendAutomation(*this, std::move(automation));
}

Document Document::withModulationDepths(std::span<const ModulationDepthEdit> edits) const {
    if (edits.empty())
        return *this;
    require(edits.size() <= modulation_.size(), "Too many modulation depth edits for this graph");

    auto root = *json::read(json_).object();
    auto routes = *root.at("modulation").array();
    std::set<std::size_t> updated;
    for (const auto& edit : edits) {
        require(std::isfinite(edit.depth), "Modulation depth must be finite");
        const auto route = std::find_if(routes.begin(), routes.end(), [&](const Value& value) {
            return value["source"].text() == edit.source && value["node"].text() == edit.node &&
                   value["parameter"].text() == edit.parameter;
        });
        require(route != routes.end(), "Unknown modulation route");
        const auto index = static_cast<std::size_t>(route - routes.begin());
        require(updated.insert(index).second, "Duplicate modulation depth edit");
        if (*(*route)["depth"].number() != edit.depth) {
            auto fields = *route->object();
            fields.at("depth") = Value(edit.depth);
            *route = Value(std::move(fields));
        }
    }
    root.at("modulation") = Value(std::move(routes));
    return parse(json::write(Value(std::move(root))));
}

Document Document::withSpeakerLayout(std::span<const std::optional<float>> degrees) const {
    auto root = *json::read(json_).object();
    const auto layout = root.find("speakerLayout");
    require(layout != root.end() && degrees.size() <= 64, "Expected a shared speaker layout with up to 64 channels");
    Value::Array speakers;
    speakers.reserve(degrees.size());
    for (const auto angle : degrees) {
        require(!angle || std::isfinite(*angle), "Speaker angle must be finite");
        speakers.push_back(angle ? Value(static_cast<double>(*angle)) : Value{});
    }
    layout->second = Value(std::move(speakers));
    return parse(json::write(Value(std::move(root))));
}

Document Document::withListenerYaw(std::span<const ListenerYawPoint> points) const {
    if (points.empty())
        return *this;
    require(listenerYaw_.has_value(), "Define a listener before scheduling yaw");
    require(points.size() <= maxAutomation - events_.size(), "Too many graph automation points");
    Value::Array automation;
    automation.reserve(points.size());
    for (const auto& point : points) {
        require(std::isfinite(point.degrees) && point.degrees >= -180 && point.degrees <= 180,
                "Listener yaw must be from -180 to 180 degrees");
        automation.emplace_back(Value::Object{
            {"sample", Value(point.sample)}, {"listenerYaw", Value(point.degrees)}});
    }
    return appendAutomation(*this, std::move(automation));
}

Document Document::withListenerYaw(double degrees, std::uint64_t sample) const {
    const ListenerYawPoint point{sample, degrees};
    return withListenerYaw({&point, 1});
}

Document Document::withIntent(std::string_view id, double normalized, std::uint64_t sample) const {
    require(std::isfinite(normalized) && normalized >= 0 && normalized <= 1,
            "Graph intent value must be between 0 and 1");
    require(std::any_of(intents_.begin(), intents_.end(), [id](const Intent& intent) { return intent.id == id; }),
            "Unknown graph intent");
    Value::Array automation;
    automation.emplace_back(Value::Object{
        {"sample", Value(sample)}, {"intent", Value(std::string(id))}, {"value", Value(normalized)}});
    return appendAutomation(*this, std::move(automation));
}

Document Document::withBrightness(std::string_view node, double value, std::uint64_t sample) const {
    const AutomationPoint point{node, "brightness", sample, value};
    return withAutomation({&point, 1});
}

Player::Player(const Document& document, const ProcessSpec& spec)
    : spec_(spec), routing_(document.routing_), latency_(document.latency_) {
    require(!document.nodes_.empty() && spec.sampleRate == document.sampleRate_ && spec.channels > 0 &&
            spec.channels <= 64 && spec.maxFrames > 0, "Incompatible graph process spec");
    require(document.requiredChannels() == 0 || spec.channels == document.requiredChannels(),
            "Audio channel count does not match the graph width");
    for (const auto& node : document.nodes_) {
        nodes_.push_back({node.id, node.kind, prepareSetting(node, node.initial, spec.sampleRate), 1,
            std::vector<dsp::Biquad>(node.kind == Document::Kind::lowpass ? spec.channels : 0), {}, {}, {}, node.channelMap, node.panSource, node.speakers, 0, {}});
        if (node.kind == Document::Kind::pan || node.kind == Document::Kind::azimuth ||
            node.kind == Document::Kind::tremolo)
            nodes_.back().pan.prepare(spec.sampleRate, .01);
        if (node.kind == Document::Kind::tremolo)
            nodes_.back().phaseStep = node.rateHz / spec.sampleRate;
        if (node.kind == Document::Kind::azimuth)
            nodes_.back().distanceGain.prepare(spec.sampleRate, .01);
        if (node.interpolatesAzimuth)
            appendKeyframe(nodes_.back().azimuthKeyframes, 0, nodes_.back().initial.pan, true);
        if (node.interpolatesDistance)
            appendKeyframe(nodes_.back().distanceKeyframes, 0, node.distanceMeters, false);
        auto& oversampling = nodes_.back().oversampling;
        switch (node.oversampling) {
        case 2: oversampling.emplace<1>(spec.channels); break;
        case 4: oversampling.emplace<2>(spec.channels); break;
        case 8: oversampling.emplace<3>(spec.channels); break;
        case 16: oversampling.emplace<4>(spec.channels); break;
        default: break;
        }
    }
    if (!document.modulation_.empty()) {
        for (const auto& route : document.modulation_)
            nodes_[route.node].modulation.push_back(route);
        const auto phaseStep = [&](double hz) {
            return static_cast<std::uint64_t>(std::ldexp(hz / spec.sampleRate, 64));
        };
        for (const auto& lfo : document.lfos_) {
            auto& prepared = lfos_.emplace_back(Lfo{lfo.waveform, lfo.followsHost ? lfo.rate : 0, {}});
            auto phase = static_cast<std::uint64_t>(std::ldexp(lfo.phase, 64));
            if (!lfo.beatSynced) {
                prepared.segments.push_back({0, phase, phaseStep(lfo.rate)});
                continue;
            }
            prepared.segments.reserve(document.tempoMap_.size());
            for (const auto& point : document.tempoMap_) {
                if (!prepared.segments.empty()) {
                    const auto& previous = prepared.segments.back();
                    phase = previous.phase + (point.sample - previous.sample) * previous.step;
                }
                prepared.segments.push_back({point.sample, phase, phaseStep(point.bpm / (60 * lfo.rate))});
            }
        }
    }
    if (document.listenerYaw_) {
        initialYaw_ = wrapAzimuth(*document.listenerYaw_);
        listener_.prepare(spec.sampleRate, .01);
        if (document.interpolateListenerYaw_)
            appendKeyframe(yawKeyframes_, 0, initialYaw_, true);
    }
    for (const auto& event : document.events_) {
        if (event.node == Document::listenerEvent) {
            const auto angle = wrapAzimuth(static_cast<float>(event.value));
            if (document.interpolateListenerYaw_) {
                appendKeyframe(yawKeyframes_, event.sample, angle, true);
                continue;
            }
            const Setting setting{1, {}, angle};
            if (!listenerEvents_.empty() && events_[listenerEvents_.back()].sample == event.sample) {
                events_[listenerEvents_.back()].setting = setting;
            } else {
                listenerEvents_.push_back(events_.size());
                events_.push_back({event.sample, event.node, setting});
            }
            continue;
        }
        if (event.distance) {
            auto& node = nodes_[event.node];
            if (!node.distanceKeyframes.empty()) {
                appendKeyframe(node.distanceKeyframes, event.sample, static_cast<float>(event.value), false);
                continue;
            }
            auto& indices = node.distanceEvents;
            const Setting setting{distanceGain(event.value), {}, 0};
            if (!indices.empty() && events_[indices.back()].sample == event.sample) {
                events_[indices.back()].setting = setting;
            } else {
                indices.push_back(events_.size());
                events_.push_back({event.sample, event.node, setting, true});
            }
            continue;
        }
        auto& node = nodes_[event.node];
        const auto setting = prepareSetting(document.nodes_[event.node], event.value, spec.sampleRate);
        if (!node.azimuthKeyframes.empty()) {
            appendKeyframe(node.azimuthKeyframes, event.sample, setting.pan, true);
            continue;
        }
        node.eventIndices.push_back(events_.size());
        events_.push_back({event.sample, event.node, setting});
    }
    if (!routing_.empty()) {
        constexpr auto scratchLimit = std::uint64_t{64} * 1024 * 1024;
        auto frames = std::uint64_t(spec.maxFrames) * nodes_.size();
        for (const auto& node : document.nodes_)
            frames += node.delayFrames;
        for (const auto& routes : routing_)
            for (const auto route : routes)
                frames += route.delay;
        const auto yawFrames = document.listenerYaw_ ? spec.maxFrames : 0;
        const auto azimuthFrames = std::any_of(nodes_.begin(), nodes_.end(),
            [](const Node& node) { return !node.azimuthKeyframes.empty(); }) ? spec.maxFrames : 0;
        const auto distanceFrames = std::any_of(nodes_.begin(), nodes_.end(),
            [](const Node& node) { return !node.distanceKeyframes.empty(); }) ? spec.maxFrames : 0;
        const auto controlFrames = lfos_.empty() ? 0 : std::uint64_t(spec.maxFrames) * (lfos_.size() + 1);
        const auto bytes = (frames * spec.channels + yawFrames + azimuthFrames + distanceFrames + controlFrames) * sizeof(float);
        require(bytes <= scratchLimit, "Parallel graph scratch audio exceeds 64 MiB");
        if (controlFrames) {
            lfoFrames_.resize(std::size_t(spec.maxFrames) * lfos_.size());
            modulationFrames_.resize(spec.maxFrames);
        }
        yawFrames_.resize(yawFrames);
        azimuthFrames_.resize(azimuthFrames);
        distanceFrames_.resize(distanceFrames);
        nodeAudio_.reserve(nodes_.size());
        for (std::size_t i = 0; i < nodes_.size(); ++i) {
            nodeAudio_.emplace_back(spec.channels, spec.maxFrames);
            nodes_[i].delay.prepare(spec.channels, document.nodes_[i].delayFrames);
        }
        routeDelays_.resize(routing_.size());
        for (std::size_t i = 0; i < routing_.size(); ++i)
            for (const auto route : routing_[i]) {
                routeDelays_[i].emplace_back();
                routeDelays_[i].back().prepare(spec.channels, route.delay);
            }
    }
    reset();
}

Player::Setting Player::prepareSetting(const Document::Node& node, double value, double rate) {
    if (node.controlKey().empty())
        return {};
    if (node.kind == Document::Kind::azimuth)
        return {distanceGain(node.distanceMeters), {}, wrapAzimuth(static_cast<float>(value))};
    if (node.kind == Document::Kind::pan || node.kind == Document::Kind::tremolo)
        return {1, {}, static_cast<float>(value)};
    if (node.kind == Document::Kind::gain || node.kind == Document::Kind::saturator)
        return {dbToGain(static_cast<float>(value)), {}, 0, static_cast<float>(value)};
    const double hz = node.cutoffMapping().map(value);
    return {1, dsp::BiquadCoefficients::lowpass(rate, hz)};
}

void Player::apply(Node& node, const Setting& setting) noexcept {
    node.gain = setting.gain;
    node.gainDb = setting.gainDb;
    if (node.kind == Document::Kind::pan || node.kind == Document::Kind::tremolo)
        node.pan.target(setting.pan);
    else if (node.kind == Document::Kind::azimuth)
        targetAzimuth(node.pan, node.azimuthTarget, setting.pan);
    for (auto& channel : node.channels)
        channel.coefficients(setting.filter);
}

void Player::reset() noexcept {
    position_ = 0;
    nextEvent_ = 0;
    listener_.reset(initialYaw_);
    targetYaw_ = initialYaw_;
    for (auto& node : nodes_) {
        apply(node, node.initial);
        node.delay.reset();
        node.pan.reset(node.initial.pan);
        node.phase = 0;
        node.distanceGain.reset(node.initial.gain);
        node.azimuthTarget = node.initial.pan;
        std::visit([](auto& states) {
            if constexpr (!std::is_same_v<std::decay_t<decltype(states)>, std::monostate>)
                for (auto& channel : states)
                    channel.reset();
        }, node.oversampling);
        for (auto& channel : node.channels)
            channel.reset();
    }
    for (auto& routes : routeDelays_)
        for (auto& delay : routes)
            delay.reset();
}

bool Player::alignTo(Player& previous) noexcept {
    position_ = previous.position_;
    const bool sameTopology = yawFrames_.empty() == previous.yawFrames_.empty() &&
        yawKeyframes_ == previous.yawKeyframes_ && routing_ == previous.routing_ &&
        lfos_ == previous.lfos_ && nodes_.size() == previous.nodes_.size() &&
        std::equal(nodes_.begin(), nodes_.end(), previous.nodes_.begin(),
            [](const Node& a, const Node& b) { return a.kind == b.kind && a.id == b.id && a.delay.frames == b.delay.frames &&
                       a.oversampling.index() == b.oversampling.index() && a.channelMap == b.channelMap &&
                       a.panSource == b.panSource && a.speakers == b.speakers && a.azimuthKeyframes == b.azimuthKeyframes &&
                       a.distanceKeyframes == b.distanceKeyframes && a.modulation == b.modulation; });
    nextEvent_ = std::lower_bound(events_.begin(), events_.end(), position_,
        [](const Event& event, std::uint64_t sample) { return event.sample < sample; }) - events_.begin();

    const auto point = std::lower_bound(listenerEvents_.begin(), listenerEvents_.end(), nextEvent_);
    const auto yaw = point == listenerEvents_.begin() ? initialYaw_ : events_[*std::prev(point)].setting.pan;
    if (sameTopology) {
        listener_ = previous.listener_;
        targetYaw_ = previous.targetYaw_;
        targetAzimuth(listener_, targetYaw_, yaw);
    } else {
        listener_.reset(yaw);
        targetYaw_ = yaw;
    }

    for (std::size_t i = 0; i < nodes_.size(); ++i) {
        auto& node = nodes_[i];
        if (sameTopology) {
            std::copy(previous.nodes_[i].channels.begin(), previous.nodes_[i].channels.end(), node.channels.begin());
            std::swap(node.delay, previous.nodes_[i].delay);
            node.oversampling.swap(previous.nodes_[i].oversampling);
            node.pan = previous.nodes_[i].pan;
            node.phase = previous.nodes_[i].phase;
            node.distanceGain = previous.nodes_[i].distanceGain;
            node.azimuthTarget = previous.nodes_[i].azimuthTarget;
        }
        const auto point = std::lower_bound(node.eventIndices.begin(), node.eventIndices.end(), nextEvent_);
        const auto& setting = point == node.eventIndices.begin() ? node.initial : events_[*std::prev(point)].setting;
        apply(node, setting);
        const auto distancePoint = std::lower_bound(node.distanceEvents.begin(), node.distanceEvents.end(), nextEvent_);
        const auto gain = distancePoint == node.distanceEvents.begin() ? node.initial.gain
            : events_[*std::prev(distancePoint)].setting.gain;
        if (sameTopology)
            node.distanceGain.target(gain);
        else {
            node.pan.reset(setting.pan);
            if (node.kind == Document::Kind::tremolo)
                node.phase = static_cast<double>(std::fmod(static_cast<long double>(position_) * node.phaseStep, 1.L));
            node.distanceGain.reset(gain);
            node.azimuthTarget = setting.pan;
        }
    }
    if (sameTopology)
        routeDelays_.swap(previous.routeDelays_);
    return sameTopology;
}

void Player::processNode(Node& node, AudioBlock<float> block, std::span<const float> yaw,
                         std::span<const float> azimuth, std::span<const float> distance,
                         std::span<float> modulation) noexcept {
    if (node.kind == Document::Kind::gain) {
        if (modulation.empty()) {
            block.multiply(node.gain);
        } else {
            for (unsigned frame = 0; frame < block.frames; ++frame) {
                const auto gain = dbToGain(std::clamp(node.gainDb + modulation[frame], -60.f, 12.f));
                for (unsigned channel = 0; channel < block.channelCount; ++channel)
                    block.channel(channel)[frame] *= gain;
            }
        }
    } else if (node.kind == Document::Kind::tremolo) {
        std::array<float*, 64> channels;
        for (unsigned c = 0; c < block.channelCount; ++c)
            channels[c] = block.channel(c);
        for (unsigned frame = 0; frame < block.frames; ++frame) {
            const auto wave = static_cast<float>(.5 - .5 * std::cos(2 * std::numbers::pi * node.phase));
            const auto baseDepth = node.pan.next();
            const auto depth = modulation.empty() ? baseDepth :
                std::clamp(baseDepth + modulation[frame], 0.f, 1.f);
            const auto gain = 1.f - depth * wave;
            for (unsigned c = 0; c < block.channelCount; ++c)
                channels[c][frame] *= gain;
            node.phase += node.phaseStep;
            if (node.phase >= 1)
                node.phase -= 1;
        }
    } else if (node.kind == Document::Kind::delay) {
        node.delay.transfer(block, block, false);
    } else if (node.kind == Document::Kind::channelMap) {
        std::array<float*, 64> channels;
        std::array<float, 64> frame;
        for (unsigned c = 0; c < block.channelCount; ++c)
            channels[c] = block.channel(c);
        for (unsigned i = 0; i < block.frames; ++i) {
            for (unsigned c = 0; c < block.channelCount; ++c)
                frame[c] = channels[c][i];
            for (unsigned c = 0; c < block.channelCount; ++c) {
                const auto source = node.channelMap[c];
                channels[c][i] = source == silentChannel ? 0.0f : frame[source];
            }
        }
    } else if (node.kind == Document::Kind::azimuth) {
        std::array<float*, 64> outputs;
        for (unsigned c = 0; c < block.channelCount; ++c)
            outputs[c] = block.channel(c);
        const auto* source = outputs[node.panSource];
        const auto& speakers = node.speakers;
        for (unsigned frame = 0; frame < block.frames; ++frame) {
            const auto gain = distance.empty() ? node.distanceGain.next() : distanceGain(distance[frame]);
            const auto value = source[frame] * gain;
            auto angle = azimuth.empty() ? wrapAzimuth(node.pan.next()) : azimuth[frame];
            if (!modulation.empty())
                angle = wrapAzimuth(std::fmod(angle + modulation[frame], 360.f));
            if (!yaw.empty())
                angle = wrapAzimuth(angle - yaw[frame]);
            const auto upper = std::upper_bound(speakers.begin(), speakers.end(), angle,
                [](float heading, const Document::Speaker& speaker) { return heading < speaker.angle; });
            const auto& left = upper == speakers.begin() ? speakers.back() : *std::prev(upper);
            const auto& right = upper == speakers.end() ? speakers.front() : *upper;
            const auto rightAngle = right.angle > left.angle ? right.angle : right.angle + 360;
            if (angle < left.angle)
                angle += 360;
            const auto blend = std::clamp((angle - left.angle) / (rightAngle - left.angle), 0.0f, 1.0f);
            for (unsigned c = 0; c < block.channelCount; ++c)
                outputs[c][frame] = 0;
            outputs[left.channel][frame] = value * std::sqrt(1 - blend);
            outputs[right.channel][frame] = value * std::sqrt(blend);
        }
    } else if (node.kind == Document::Kind::pan) {
        const auto* source = block.channel(node.panSource);
        auto* left = block.channel(0);
        auto* right = block.channel(1);
        for (unsigned i = 0; i < block.frames; ++i) {
            const float input = source[i];
            const float basePan = node.pan.next();
            const float pan = modulation.empty() ? basePan : basePan + modulation[i];
            const float rightPower = .5f * (1 + std::clamp(pan, -1.f, 1.f));
            left[i] = input * std::sqrt(1 - rightPower);
            right[i] = input * std::sqrt(rightPower);
        }
    } else if (node.kind == Document::Kind::saturator) {
        // Convert the shared frame offsets once. Each host frame's drive is
        // held across its oversampled subframes and reused for every channel.
        for (auto& offset : modulation)
            offset = dbToGain(std::clamp(node.gainDb + offset, 0.f, 36.f));
        std::visit([&](auto& states) {
            for (unsigned c = 0; c < block.channelCount; ++c) {
                auto* samples = block.channel(c);
                for (unsigned i = 0; i < block.frames; ++i) {
                    const auto drive = double(modulation.empty() ? node.gain : modulation[i]);
                    const auto saturate = [drive](double input) noexcept { return std::tanh(input * drive); };
                    if constexpr (std::is_same_v<std::decay_t<decltype(states)>, std::monostate>)
                        samples[i] = static_cast<float>(saturate(samples[i]));
                    else
                        samples[i] = static_cast<float>(states[c].process(samples[i], saturate));
                }
            }
        }, node.oversampling);
    } else {
        for (unsigned c = 0; c < block.channelCount; ++c) {
            auto* samples = block.channel(c);
            for (unsigned i = 0; i < block.frames; ++i)
                samples[i] = node.channels[c].process(samples[i]);
        }
    }
}

void Player::FixedDelay::prepare(unsigned channels, std::uint32_t delay) {
    frames = delay;
    if (frames)
        storage.resize(channels, frames);
}

void Player::FixedDelay::transfer(AudioBlock<float> target, AudioBlock<float> source,
                                  bool add) noexcept {
    for (unsigned c = 0; c < target.channelCount; ++c) {
        auto* out = target.channel(c);
        const auto* input = source.channel(c);
        if (!frames) {
            if (add) {
                for (unsigned frame = 0; frame < target.frames; ++frame)
                    out[frame] += input[frame];
            } else if (out != input) {
                std::copy_n(input, target.frames, out);
            }
            continue;
        }

        auto* ring = storage.getWritePointer(c);
        auto write = cursor;
        auto filled = valid;
        for (unsigned frame = 0; frame < target.frames; ++frame) {
            const float sample = input[frame];
            const float delayed = filled == frames ? ring[write] : 0.0f;
            ring[write] = sample;
            if (++write == frames)
                write = 0;
            if (filled < frames)
                ++filled;
            if (add)
                out[frame] += delayed;
            else
                out[frame] = delayed;
        }
    }
    if (frames) {
        cursor = static_cast<std::uint32_t>((std::uint64_t(cursor) + target.frames) % frames);
        valid += std::min(target.frames, frames - valid);
    }
}

void Player::mix(AudioBlock<float> target, AudioBlock<float> input,
                 std::size_t destination) noexcept {
    const auto& routes = routing_[destination];
    for (std::size_t i = 0; i < routes.size(); ++i) {
        const auto source = routes[i].source;
        const auto block = source == 0 ? input : nodeAudio_[source - 1].block().slice(0, target.frames);
        routeDelays_[destination][i].transfer(target, block, i != 0);
    }
}

void Player::appendKeyframe(std::vector<SpatialKeyframe>& points, std::uint64_t sample, float value, bool angular) {
    if (!points.empty() && points.back().sample == sample)
        points.back().value = value;
    else
        points.push_back({sample, value});
    if (points.size() > 1) {
        auto& previous = points[points.size() - 2];
        const auto delta = value - previous.value;
        previous.deltaToNext = angular ? std::remainder(delta, 360.f) : delta;
    }
}

void Player::fillKeyframes(std::span<const SpatialKeyframe> points, std::span<float> output,
                           std::uint64_t sample, bool angular) noexcept {
    auto next = std::upper_bound(points.begin(), points.end(), sample,
        [](std::uint64_t frame, const SpatialKeyframe& point) { return frame < point.sample; });
    for (auto& angle : output) {
        if (next != points.end() && sample == next->sample)
            ++next;
        const auto& previous = *std::prev(next);
        if (next == points.end()) {
            angle = previous.value;
        } else {
            const auto fraction = double(sample - previous.sample) / double(next->sample - previous.sample);
            const auto value = static_cast<float>(previous.value + previous.deltaToNext * fraction);
            angle = angular ? wrapAzimuth(value) : value;
        }
        ++sample;
    }
}

float Player::Lfo::value(std::uint64_t phase) const noexcept {
    if (waveform == Document::LfoWaveform::square)
        return (phase >> 63) ? -1.f : 1.f;
    const auto cycle = std::ldexp(double(phase >> 11), -53);
    if (waveform == Document::LfoWaveform::triangle) {
        if (cycle < .25)
            return static_cast<float>(4 * cycle);
        if (cycle < .75)
            return static_cast<float>(4 * (.5 - cycle));
        return static_cast<float>(4 * (cycle - 1));
    }
    return static_cast<float>(std::sin(2 * std::numbers::pi * cycle));
}

void Player::Lfo::fill(std::span<float> output, std::uint64_t sample) const noexcept {
    auto next = std::upper_bound(segments.begin(), segments.end(), sample,
        [](std::uint64_t frame, const Segment& segment) { return frame < segment.sample; });
    while (!output.empty()) {
        const auto& segment = *std::prev(next);
        // Unsigned phase wraps modulo one cycle. Each segment starts with the
        // exact accumulated phase of the preceding tempo.
        auto phase = segment.phase + (sample - segment.sample) * segment.step;
        const auto count = next == segments.end() ? output.size() :
            static_cast<std::size_t>(std::min<std::uint64_t>(output.size(), next->sample - sample));
        for (auto& frame : output.first(count)) {
            frame = value(phase);
            phase += segment.step;
        }
        output = output.subspan(count);
        sample += count;
        if (next != segments.end() && sample == next->sample)
            ++next;
    }
}

void Player::Lfo::fillFromHost(std::span<float> output, const Transport& host,
                               double rate, std::uint64_t offset) const noexcept {
    auto cycle = std::fmod(host.beatPosition, hostBeatsPerCycle) / hostBeatsPerCycle;
    if (cycle < 0)
        cycle += 1;
    const auto anchor = cycle < 1 ? static_cast<std::uint64_t>(std::ldexp(cycle, 64)) : 0;
    const auto step = host.playing ? static_cast<std::uint64_t>(
        std::ldexp(host.tempo / (60 * hostBeatsPerCycle * rate), 64)) : 0;
    auto phase = segments.front().phase + anchor + offset * step;
    for (auto& frame : output) {
        frame = value(phase);
        phase += step;
    }
}

void Player::processSegment(AudioBlock<float> block, std::uint64_t sample, const Transport* host) noexcept {
    if (routing_.empty()) {
        for (auto& node : nodes_)
            processNode(node, block);
        return;
    }

    const auto yaw = std::span(yawFrames_).first(yawFrames_.empty() ? 0 : block.frames);
    if (yawKeyframes_.empty()) {
        for (auto& angle : yaw)
            angle = wrapAzimuth(listener_.next());
    } else {
        fillKeyframes(yawKeyframes_, yaw, sample, true);
    }

    for (std::size_t source = 0; source < lfos_.size(); ++source) {
        const auto& lfo = lfos_[source];
        const auto output = std::span(lfoFrames_).subspan(source * spec_.maxFrames, block.frames);
        if (host && lfo.hostBeatsPerCycle > 0)
            lfo.fillFromHost(output, *host, spec_.sampleRate, sample - position_);
        else
            lfo.fill(output, sample);
    }

    for (std::size_t i = 0; i < nodes_.size(); ++i) {
        const auto scratch = nodeAudio_[i].block().slice(0, block.frames);
        mix(scratch, block, i);
        auto& node = nodes_[i];
        const auto azimuth = std::span(azimuthFrames_).first(node.azimuthKeyframes.empty() ? 0 : block.frames);
        if (!azimuth.empty())
            fillKeyframes(node.azimuthKeyframes, azimuth, sample, true);
        const auto distance = std::span(distanceFrames_).first(node.distanceKeyframes.empty() ? 0 : block.frames);
        if (!distance.empty())
            fillKeyframes(node.distanceKeyframes, distance, sample, false);
        auto modulation = std::span(modulationFrames_).first(node.modulation.empty() ? 0 : block.frames);
        std::fill(modulation.begin(), modulation.end(), 0.f);
        for (const auto& route : node.modulation) {
            const auto* wave = lfoFrames_.data() + route.source * spec_.maxFrames;
            for (unsigned frame = 0; frame < block.frames; ++frame)
                modulation[frame] += static_cast<float>(route.depth) * wave[frame];
        }
        processNode(node, scratch, yaw, azimuth, distance, modulation);
    }
    mix(block, block, nodes_.size());
}

bool Player::process(AudioBlock<float> block) noexcept {
    return process(block, Transport{});
}

bool Player::process(AudioBlock<float> block, const Transport& transport) noexcept {
    if (block.channelCount != spec_.channels || block.frames > spec_.maxFrames ||
        block.frames > std::numeric_limits<std::uint64_t>::max() - position_ ||
        block.frames > std::numeric_limits<std::uint32_t>::max() - block.offset)
        return false;
    ScopedNoDenormals denormals;
    const auto* host = transport.tempoValid && transport.beatPositionValid &&
        std::isfinite(transport.beatPosition) && transport.tempo >= 1 && transport.tempo <= 1000 ?
        &transport : nullptr;
    std::uint32_t offset = 0;
    const auto end = position_ + block.frames;
    while (nextEvent_ < events_.size() && events_[nextEvent_].sample < end) {
        const auto& event = events_[nextEvent_++];
        const auto frame = static_cast<std::uint32_t>(event.sample - position_);
        if (frame != offset)
            processSegment(block.slice(offset, frame - offset), position_ + offset, host);
        if (event.node == Document::listenerEvent)
            targetAzimuth(listener_, targetYaw_, event.setting.pan);
        else if (event.distance)
            nodes_[event.node].distanceGain.target(event.setting.gain);
        else
            apply(nodes_[event.node], event.setting);
        offset = frame;
    }
    if (offset < block.frames)
        processSegment(block.slice(offset, block.frames - offset), position_ + offset, host);
    position_ = end;
    return true;
}

LiveSession::LiveSession(Document document, const ProcessSpec& spec, std::uint32_t fadeFrames)
    : document_(std::move(document)), spec_(spec), fadeFrames_(fadeFrames), latency_(document_.latency()) {
    if (!publish(document_))
        throw std::logic_error("Could not publish initial graph");
    exchange_.audioUpdate();
    scratch_.resize(spec_.channels, static_cast<std::uint32_t>(
        std::min<std::uint64_t>(spec_.maxFrames, std::uint64_t(latency_) + fadeFrames_)));
    history_.reserve(historyLimit);
}

bool LiveSession::publish(const Document& document) {
    require(document.latency() == latency_, "Changing graph latency requires a new playback session");
    auto player = std::make_unique<Player>(document, spec_);
    return exchange_.publishLatest(player, [](Player&, std::uint64_t) noexcept {}) != 0;
}

bool LiveSession::replace(Document replacement) {
    if (!publish(replacement))
        return false;
    if (history_.size() == historyLimit)
        history_.erase(history_.begin());
    history_.push_back(std::move(document_));
    document_ = std::move(replacement);
    return true;
}

bool LiveSession::undo() {
    if (history_.empty() || !publish(history_.back()))
        return false;
    document_ = std::move(history_.back());
    history_.pop_back();
    return true;
}

bool LiveSession::setAutomation(std::span<const AutomationPoint> points) {
    return !points.empty() && replace(document_.withAutomation(points));
}

bool LiveSession::setModulationDepths(std::span<const ModulationDepthEdit> edits) {
    if (edits.empty())
        return false;
    auto replacement = document_.withModulationDepths(edits);
    return replacement.serialize() != document_.serialize() && replace(std::move(replacement));
}

bool LiveSession::setListenerYaw(std::span<const ListenerYawPoint> points) {
    return !points.empty() && replace(document_.withListenerYaw(points));
}

bool LiveSession::setListenerYaw(double degrees, std::uint64_t sample) {
    const ListenerYawPoint point{sample, degrees};
    return setListenerYaw({&point, 1});
}

bool LiveSession::setIntent(std::string_view id, double normalized, std::uint64_t sample) {
    return replace(document_.withIntent(id, normalized, sample));
}

bool LiveSession::setBrightness(std::string_view node, double value, std::uint64_t sample) {
    const AutomationPoint point{node, "brightness", sample, value};
    return setAutomation({&point, 1});
}

bool LiveSession::updatePending() const noexcept {
    return exchange_.activeGeneration() != exchange_.requestedGeneration();
}

bool LiveSession::process(AudioBlock<float> block) noexcept {
    return process(block, Transport{});
}

bool LiveSession::process(AudioBlock<float> block, const Transport& transport) noexcept {
    const auto position = position_.load(std::memory_order_relaxed);
    if (block.channelCount != spec_.channels || block.frames > spec_.maxFrames ||
        block.frames > std::numeric_limits<std::uint64_t>::max() - position ||
        block.frames > std::numeric_limits<std::uint32_t>::max() - block.offset)
        return false;
    if (block.frames == 0)
        return true;

    auto* active = exchange_.audioCurrent();
    if (warming_ == 0 && remaining_ == 0 && exchange_.audioReleasePrevious()) {
        auto* next = exchange_.audioUpdateRetainingPrevious();
        if (next != active) {
            const bool sameTopology = next->alignTo(*active);
            if (!sameTopology) {
                warming_ = latency_;
                remaining_ = fadeFrames_;
            }
            active = next;
        }
    }

    const auto count = static_cast<std::uint32_t>(
        std::min<std::uint64_t>(std::uint64_t(warming_) + remaining_, block.frames));
    const auto warm = std::min(warming_, count);
    if (count != 0) {
        const auto oldBlock = scratch_.block().slice(0, count);
        for (unsigned channel = 0; channel < block.channelCount; ++channel)
            std::copy_n(block.channel(channel), count, oldBlock.channel(channel));
        exchange_.audioPrevious()->process(oldBlock, transport);
        active->process(block, transport);
        for (unsigned channel = 0; channel < block.channelCount; ++channel) {
            auto* current = block.channel(channel);
            const auto* old = oldBlock.channel(channel);
            std::copy_n(old, warm, current);
            for (std::uint32_t frame = warm; frame < count; ++frame) {
                const auto weight = float(fadeFrames_ - remaining_ + frame - warm + 1) / float(fadeFrames_);
                current[frame] = std::lerp(old[frame], current[frame], weight);
            }
        }
        warming_ -= warm;
        remaining_ -= count - warm;
    } else {
        active->process(block, transport);
    }
    if (warming_ == 0 && remaining_ == 0)
        exchange_.audioReleasePrevious();
    position_.store(position + block.frames, std::memory_order_release);
    return true;
}

} // namespace cell::graph
