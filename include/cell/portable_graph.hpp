// Original Cell Framework implementation; see LICENSE.
#pragma once

#include <cell/audio.hpp>
#include <cell/dsp.hpp>
#include <cell/oversampling.hpp>
#include <cell/prepared_exchange.hpp>
#include <optional>
#include <string>
#include <string_view>
#include <variant>

namespace cell::graph {

class Player;

struct ControlMapping {
    // A normalized control maps exponentially from minimum to maximum.
    std::string_view target;
    std::string_view unit;
    double minimum, maximum;
    double map(double normalized) const noexcept;
};

struct Control {
    std::string node;
    std::string_view key, meaning, unit;
    double minimum, maximum, initial;
    std::optional<ControlMapping> mapping;
};

struct AzimuthLayout {
    std::string node;
    unsigned sourceChannel;
    // Output channel order. An empty slot is silent on this branch.
    std::vector<std::optional<float>> speakerDegrees;
    float distanceMeters = 1;
    bool interpolatesAzimuth{}, interpolatesDistance{};
};

struct ListenerYawPoint {
    std::uint64_t sample;
    double degrees;
};

struct AutomationPoint {
    std::string_view node, parameter;
    std::uint64_t sample;
    double value;
};

struct ModulationDepthEdit {
    std::string_view source, node, parameter;
    double depth;
};

struct IntentBinding {
    enum class Interpolation { linear, shortestArc };

    std::string node, parameter;
    double atZero, atOne;
    Interpolation interpolation = Interpolation::linear;
};

struct Intent {
    std::string id, name;
    std::vector<IntentBinding> bindings;
};

// Versioned graph data. Parsing and serialization are control-thread operations.
class Document {
public:
    static Document parse(std::string_view json);
    const std::string& serialize() const noexcept { return json_; }
    std::vector<Control> controls() const;
    std::vector<AzimuthLayout> azimuthLayouts() const;
    std::optional<float> listenerYaw() const noexcept { return listenerYaw_; }
    std::span<const Intent> intents() const noexcept { return intents_; }
    Document withSpeakerLayout(std::span<const std::optional<float>> degrees) const;
    Document withListenerYaw(std::span<const ListenerYawPoint> points) const;
    Document withListenerYaw(double degrees, std::uint64_t sample) const;
    Document withIntent(std::string_view id, double normalized, std::uint64_t sample) const;
    // Apply a batch as one document edit. New points at an existing sample
    // come after saved points, in the order in which the caller supplied them.
    Document withAutomation(std::span<const AutomationPoint>) const;
    // Change existing routes as one edit. Duplicate or invalid entries reject the batch.
    Document withModulationDepths(std::span<const ModulationDepthEdit>) const;
    Document withBrightness(std::string_view node, double value, std::uint64_t sample) const;
    unsigned sampleRate() const noexcept { return sampleRate_; }
    unsigned requiredChannels() const noexcept { return requiredChannels_; }
    std::uint32_t latency() const noexcept { return latency_; }
    // Known finite ringing beyond the graph's declared latency.
    std::uint32_t tail() const noexcept { return tail_; }
    std::uint32_t renderFlushFrames() const noexcept { return latency_ + tail_; }
private:
    enum class Kind { gain, lowpass, delay, saturator, channelMap, pan, azimuth, tremolo };
    struct Speaker {
        float angle;
        unsigned channel;
        bool operator==(const Speaker&) const = default;
    };
    struct Node {
        std::string id;
        Kind kind;
        double initial;
        double minimum;
        double maximum;
        std::uint32_t delayFrames{};
        unsigned oversampling = 1;
        std::vector<std::uint8_t> channelMap{};
        unsigned panSource{};
        std::vector<Speaker> speakers{};
        float distanceMeters = 1;
        double rateHz{};
        bool distanceControl{};
        bool interpolatesAzimuth{}, interpolatesDistance{};
        std::uint32_t latency() const noexcept;
        std::string_view controlKey() const noexcept;
        double controlMin() const noexcept { return kind == Kind::lowpass ? 0 : minimum; }
        double controlMax() const noexcept { return kind == Kind::lowpass ? 1 : maximum; }
        ControlMapping cutoffMapping() const noexcept { return {"cutoff", "Hz", minimum, maximum}; }
    };
    struct ModulationRoute {
        std::size_t source, node;
        double depth;
        bool operator==(const ModulationRoute&) const = default;
    };
    enum class LfoWaveform { sine, triangle, square };
    struct LfoDefinition {
        double rate, phase;
        bool beatSynced, followsHost;
        LfoWaveform waveform;
    };
    struct TempoPoint {
        std::uint64_t sample;
        double bpm;
    };
    std::vector<TempoPoint> tempoMap_;
    std::vector<LfoDefinition> lfos_;
    std::vector<ModulationRoute> modulation_;
    struct Event {
        std::uint64_t sample;
        std::size_t node;
        double value;
        bool distance{};
    };
    static constexpr std::size_t listenerEvent = std::size_t(-1);
    std::optional<float> listenerYaw_;
    bool interpolateListenerYaw_{};
    Document() = default;
    std::string json_;
    struct Route {
        std::size_t source;
        std::uint32_t delay{};
        bool operator==(const Route&) const = default;
    };
    unsigned sampleRate_{}, requiredChannels_{};
    std::uint32_t latency_{}, tail_{};
    std::vector<Node> nodes_;
    std::vector<Event> events_;
    std::vector<Intent> intents_;
    // Empty for the in-place V1 chain. V2 sources use 0 for host input,
    // followed by one-based indexes into the ordered nodes.
    std::vector<std::vector<Route>> routing_;
    friend class Player;
};

// Construct off audio. A player owns its filter state and starts at sample zero.
// Use one processing thread per player. Reset rewinds automation and filter state.
class Player {
public:
    Player(const Document&, const ProcessSpec&);
    void reset() noexcept;
    std::uint32_t latency() const noexcept { return latency_; }
    // A layout/size mismatch leaves the block and playback position untouched.
    bool process(AudioBlock<float>) noexcept;
    bool process(AudioBlock<float>, const Transport&) noexcept;
private:
    struct Setting {
        float gain = 1;
        dsp::BiquadCoefficients filter;
        float pan{};
        float gainDb{};
    };
    struct FixedDelay {
        AudioBuffer<float> storage;
        std::uint32_t frames{}, cursor{}, valid{};
        void prepare(unsigned channels, std::uint32_t delay);
        void reset() noexcept { cursor = valid = 0; }
        void transfer(AudioBlock<float> target, AudioBlock<float> source, bool add) noexcept;
    };
    using Oversampling = std::variant<std::monostate, std::vector<dsp::Oversampler<1>>,
        std::vector<dsp::Oversampler<2>>, std::vector<dsp::Oversampler<3>>,
        std::vector<dsp::Oversampler<4>>>;
    struct SpatialKeyframe {
        std::uint64_t sample;
        float value;
        float deltaToNext{};
        bool operator==(const SpatialKeyframe&) const = default;
    };
    struct Node {
        std::string id;
        Document::Kind kind;
        Setting initial;
        float gain = 1;
        std::vector<dsp::Biquad> channels;
        std::vector<std::size_t> eventIndices;
        FixedDelay delay;
        Oversampling oversampling;
        std::vector<std::uint8_t> channelMap;
        unsigned panSource{};
        std::vector<Document::Speaker> speakers;
        float azimuthTarget{};
        dsp::SmoothedValue pan;
        dsp::SmoothedValue distanceGain{};
        std::vector<std::size_t> distanceEvents{};
        std::vector<SpatialKeyframe> azimuthKeyframes{}, distanceKeyframes{};
        double phase{}, phaseStep{};
        float gainDb{};
        std::vector<Document::ModulationRoute> modulation{};
    };
    struct Event {
        std::uint64_t sample;
        std::size_t node;
        Setting setting;
        bool distance{};
    };
    friend class LiveSession;
    bool alignTo(Player& previous) noexcept;
    static Setting prepareSetting(const Document::Node&, double, double);
    static void apply(Node&, const Setting&) noexcept;
    static void processNode(Node&, AudioBlock<float>, std::span<const float> yaw = {},
                            std::span<const float> azimuth = {}, std::span<const float> distance = {},
                            std::span<float> modulation = {}) noexcept;
    void processSegment(AudioBlock<float>, std::uint64_t sample, const Transport* host) noexcept;
    static void appendKeyframe(std::vector<SpatialKeyframe>&, std::uint64_t sample, float value, bool angular);
    static void fillKeyframes(std::span<const SpatialKeyframe>, std::span<float>, std::uint64_t sample, bool angular) noexcept;
    void mix(AudioBlock<float>, AudioBlock<float>, std::size_t destination) noexcept;
    ProcessSpec spec_;
    std::vector<Node> nodes_;
    std::vector<Event> events_;
    std::vector<std::vector<Document::Route>> routing_;
    std::vector<std::vector<FixedDelay>> routeDelays_;
    std::uint32_t latency_{};
    std::vector<AudioBuffer<float>> nodeAudio_;
    dsp::SmoothedValue listener_;
    float initialYaw_{}, targetYaw_{};
    std::vector<float> yawFrames_, azimuthFrames_, distanceFrames_;
    struct Lfo {
        struct Segment {
            std::uint64_t sample, phase, step;
            bool operator==(const Segment&) const = default;
        };
        Document::LfoWaveform waveform;
        double hostBeatsPerCycle;
        std::vector<Segment> segments;
        bool operator==(const Lfo&) const = default;
        float value(std::uint64_t phase) const noexcept;
        void fill(std::span<float>, std::uint64_t sample) const noexcept;
        void fillFromHost(std::span<float>, const Transport&, double rate, std::uint64_t offset) const noexcept;
    };
    std::vector<Lfo> lfos_;
    std::vector<float> lfoFrames_, modulationFrames_;
    std::vector<std::size_t> listenerEvents_;
    std::vector<SpatialKeyframe> yawKeyframes_;
    std::uint64_t position_{};
    std::size_t nextEvent_{};
};

// One serialized control producer and one audio consumer. Stop both before
// destruction. Control reads the latest accepted document and writes edits.
// Audio owns the running position. The session begins with the supplied graph.
class LiveSession {
public:
    LiveSession(Document, const ProcessSpec&, std::uint32_t fadeFrames = 128);
    // Invalid documents/specs or a change to session latency throw. False means no change (backpressure or
    // empty history). A successful edit may supersede an unplayed edit.
    bool replace(Document);
    bool undo();
    // Publish a batch as one undoable edit.
    bool setAutomation(std::span<const AutomationPoint>);
    bool setModulationDepths(std::span<const ModulationDepthEdit>);
    bool setListenerYaw(std::span<const ListenerYawPoint> points);
    bool setListenerYaw(double degrees, std::uint64_t sample);
    bool setIntent(std::string_view id, double normalized, std::uint64_t sample);
    // Schedule a brightness point. Publish before its sample to hear it there.
    bool setBrightness(std::string_view node, double value, std::uint64_t sample);
    const Document& document() const noexcept { return document_; }
    std::uint32_t latency() const noexcept { return latency_; }
    bool updatePending() const noexcept;
    std::uint64_t nextSample() const noexcept { return position_.load(std::memory_order_acquire); }
    std::size_t reclaim() noexcept { return exchange_.reclaim(); }

    // Audio thread only. The supplied input may loop independently of this clock.
    bool process(AudioBlock<float>) noexcept;
    bool process(AudioBlock<float>, const Transport&) noexcept;
private:
    bool publish(const Document&);
    static constexpr std::size_t historyLimit = 16;
    Document document_;
    std::vector<Document> history_;
    ProcessSpec spec_;
    std::uint32_t fadeFrames_;
    const std::uint32_t latency_;
    std::uint32_t remaining_{}, warming_{};
    std::atomic<std::uint64_t> position_{};
    AudioBuffer<float> scratch_;
    PreparedExchange<Player> exchange_;
};

} // namespace cell::graph
