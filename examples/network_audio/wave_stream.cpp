#include "udp_socket.hpp"
#include "latency_measure.hpp"

#include <cell/audio_file.hpp>
#include <cell/audio_device.hpp>
#include <cell/files.hpp>
#include <cell/network_audio_receiver.hpp>
#include <cell/network_audio_sender.hpp>
#include <cell/uuid.hpp>

#include <charconv>
#include <cmath>
#include <csignal>
#include <iomanip>
#include <iostream>
#include <stdexcept>
#include <thread>

namespace {
using namespace cell;
using namespace cell::network_audio;
using cell::network_audio::example::UdpSocket;
using namespace std::chrono_literals;
using Clock = std::chrono::steady_clock;
constexpr unsigned defaultRate = 48000;
constexpr unsigned minStreamRate = 8000;
constexpr unsigned maxStreamRate = 96000;
constexpr int blockFrames = 120;
constexpr int maxReceiveBatch = 128;
constexpr std::int64_t maxStreamFrames(unsigned sampleRate) noexcept {
    return 24 * 60 * 60LL * sampleRate;
}
constexpr auto defaultBudget = 20ms;
constexpr auto idleTimeout = 2s;
constexpr auto minBudget = 5ms;
constexpr auto maxBudget = 150ms;
using Stereo = Frame<2>;
constexpr auto bufferedFrames = maxStreamRate * (2 * maxBudget.count()) / 1000 + 2 * blockFrames;
using StereoReceiver = Receiver<2, 16384, bufferedFrames>;

struct LiveOptions {
    audio::DeviceConfig device;
    std::optional<sockaddr_in> peer;
    std::chrono::milliseconds statsInterval{};
};

struct Monitor;

#ifdef CELL_NETWORK_AUDIO_DEVICE
static_assert(std::atomic<bool>::is_always_lock_free);
std::atomic<bool> stopRequested{false};
constexpr auto packetStallThreshold = 250ms;

void requestStop(int) { stopRequested.store(true, std::memory_order_relaxed); }

void installStopHandler() {
    if (std::signal(SIGINT, requestStop) == SIG_ERR)
        throw std::runtime_error("Could not install audio stop handler");
}

class StatsTicker {
public:
    explicit StatsTicker(std::chrono::milliseconds interval) noexcept
        : interval_(interval), next_(Clock::now() + interval) {}

    bool due() noexcept {
        if (interval_ == 0ms) return false;
        const auto now = Clock::now();
        if (now < next_) return false;
        next_ = now + interval_;
        return true;
    }

private:
    std::chrono::milliseconds interval_;
    Clock::time_point next_;
};

void appendDeviceStats(std::ostream& output, const audio::DeviceStats& stats) {
    output << ",\"device_running\":" << (stats.running ? "true" : "false")
           << ",\"device_interrupted\":" << (stats.interrupted ? "true" : "false")
           << ",\"device_callbacks\":" << stats.callbacks
           << ",\"device_frames\":" << stats.frames
           << ",\"callback_overruns\":" << stats.callbackOverruns
           << ",\"max_callback_ns\":" << stats.maximumCallbackNanoseconds;
}

void appendPeer(std::ostream& output, const sockaddr_in* peer) {
    std::array<char, INET_ADDRSTRLEN> ipv4{};
    if (peer && inet_ntop(AF_INET, &peer->sin_addr, ipv4.data(), ipv4.size()))
        output << '"' << ipv4.data() << ':' << ntohs(peer->sin_port) << '"';
    else
        output << "null";
}

struct LiveProgress {
    std::uint64_t limit; // Zero means continuous.
    std::uint64_t processed{};
    std::atomic<bool> finished{false};

    unsigned activeFrames(unsigned requested) const noexcept {
        if (!limit) return requested;
        return static_cast<unsigned>(std::min<std::uint64_t>(requested, limit - processed));
    }

    void advance(unsigned count) noexcept {
        processed += count;
        if (limit && processed == limit) finished.store(true, std::memory_order_release);
    }
};

struct Monitor {
    Monitor(std::optional<StereoReceiver>& stream, std::uint64_t frames, const LiveOptions& options)
        : receiver(stream), progress{frames}, statsInterval(options.statsInterval) {
        const auto format = device.open(options.device, callback, this);
        if (!device.isOpen() || format.sampleRate != options.device.sampleRate || format.outputChannels != 2)
            throw std::runtime_error("Could not open stereo output at the requested sample rate: " + device.error());
    }

    std::optional<StereoReceiver>& receiver;
    LiveProgress progress;
    const std::chrono::milliseconds statsInterval;
    audio::Device device;

    static void callback(void* context, float* output, const float*, unsigned frames) noexcept {
        auto& monitor = *static_cast<Monitor*>(context);
        std::array<Stereo, blockFrames> block;
        const auto active = monitor.progress.activeFrames(frames);
        for (unsigned offset = 0; offset < active;) {
            const auto count = std::min<unsigned>(block.size(), active - offset);
            monitor.receiver->render(std::span(block).first(count));
            for (unsigned index = 0; index < count; ++index) {
                *output++ = block[index][0];
                *output++ = block[index][1];
            }
            offset += count;
        }
        std::fill_n(output, 2 * static_cast<std::size_t>(frames - active), 0.f);
        monitor.progress.advance(active);
    }
};

struct Capture {
    Capture(std::uint64_t frames, unsigned channels, unsigned sampleRate) noexcept
        : sender(sampleRate, 0), progress{frames}, inputChannels(channels) {}
    Sender<2, 8192> sender;
    LiveProgress progress;
    const unsigned inputChannels;
    std::uint64_t missingFrames{};
    std::uint64_t droppedFrames{};

    static void callback(void* context, float* output, const float* input, unsigned frames) noexcept {
        auto& capture = *static_cast<Capture*>(context);
        // An optional duplex output stays silent.
        if (output) std::fill_n(output, 2 * static_cast<std::size_t>(frames), 0.f);
        const auto active = capture.progress.activeFrames(frames);
        if (!active) return;
        if (!input) capture.missingFrames += active;

        std::array<Stereo, blockFrames> block;
        for (unsigned offset = 0; offset < active;) {
            const auto count = std::min<unsigned>(block.size(), active - offset);
            for (unsigned index = 0; index < count; ++index) {
                block[index] = input ? Stereo{input[0], input[capture.inputChannels - 1]} : Stereo{};
                if (input) input += capture.inputChannels;
            }
            if (!capture.sender.capture(std::span(block).first(count))) capture.droppedFrames += count;
            offset += count;
        }
        capture.progress.advance(active);
    }
};
#endif

std::int64_t number(std::string_view text, std::int64_t maximum, std::int64_t minimum = 1) {
    std::int64_t value{};
    const auto parsed = std::from_chars(text.data(), text.data() + text.size(), value);
    if (parsed.ec != std::errc{} || parsed.ptr != text.data() + text.size() ||
        value < minimum || value > maximum) throw std::invalid_argument("Numeric argument out of range");
    return value;
}

unsigned streamRate(std::string_view text) {
    return static_cast<unsigned>(number(text, maxStreamRate, minStreamRate));
}

sockaddr_in endpoint(const char* host, const char* port, bool allowEphemeral = false) {
    sockaddr_in result{};
    result.sin_family = AF_INET;
    result.sin_port = htons(static_cast<std::uint16_t>(number(port, 65535, allowEphemeral ? 0 : 1)));
    if (inet_pton(AF_INET, host, &result.sin_addr) != 1)
        throw std::invalid_argument("Enter a numeric IPv4 address");
    return result;
}

sockaddr_in peerOption(int argc, char** argv, int first) {
    if (argc - first < 3 || std::string_view(argv[first]) != "--peer")
        throw std::invalid_argument("Expected --peer IPV4 PORT");
    return endpoint(argv[first + 1], argv[first + 2]);
}

Clock::duration frameTime(std::int64_t frames, unsigned sampleRate) {
    return std::chrono::duration_cast<Clock::duration>(std::chrono::duration<double>(
        static_cast<double>(frames) / sampleRate));
}

void connectSender(UdpSocket& socket, const sockaddr_in& address, const std::optional<sockaddr_in>& local) {
    if (local) socket.bind(*local);
    socket.connect(address);
}

#ifdef CELL_NETWORK_AUDIO_DEVICE
std::uint64_t liveFrameLimit(std::string_view argument, unsigned sampleRate) {
    if (argument == "continuous") return 0;
    return static_cast<std::uint64_t>(number(argument, maxStreamFrames(sampleRate)));
}

audio::Backend selectBackend(std::string_view name) {
    if (name == "auto") return audio::Backend::automatic;
    if (name == "alsa") return audio::Backend::alsa;
    if (name == "pulse") return audio::Backend::pulse;
    if (name == "jack") return audio::Backend::jack;
    if (name == "coreaudio") return audio::Backend::coreAudio;
    if (name == "wasapi") return audio::Backend::wasapi;
    throw std::invalid_argument("Expected audio backend auto, alsa, pulse, jack, coreaudio, or wasapi");
}

void listDevices(audio::Backend backend) {
    audio::Device device;
    const auto devices = device.enumerate(backend);
    std::cout << "Backend: " << audio::backendName(devices.backend) << '\n';
    for (const auto& [direction, endpoints] :
         {std::pair{"Input", &devices.inputs}, std::pair{"Output", &devices.outputs}}) {
        for (const auto& endpoint : *endpoints)
            std::cout << direction << '\t' << endpoint.id << '\t' << endpoint.name
                      << (endpoint.defaultDevice ? " (default)" : "") << '\n';
    }
}

LiveOptions deviceOptions(int argc, char** argv, int first, bool capture) {
    LiveOptions options;
    auto& config = options.device;
    config.sampleRate = defaultRate;
    config.inputChannels = capture ? 2 : 0;
    for (int i = first; i < argc;) {
        const std::string_view flag = argv[i];
        if (capture && flag == "--mono") {
            config.inputChannels = 1;
            ++i;
            continue;
        }
        if (capture && flag == "--capture-only") {
            config.outputChannels = 0;
            ++i;
            continue;
        }
        if (!capture && flag == "--peer") {
            if (options.peer) throw std::invalid_argument("Specify --peer once");
            options.peer = peerOption(argc, argv, i);
            i += 3;
            continue;
        }
        if (flag != "--backend" && flag != "--output" && flag != "--stats-ms" && flag != "--rate" &&
            !(capture && flag == "--input"))
            throw std::invalid_argument("Unknown audio device option: " + std::string(flag));
        if (i + 1 >= argc || !*argv[i + 1] || std::string_view(argv[i + 1]).starts_with("--"))
            throw std::invalid_argument("Missing value for " + std::string(flag));
        if (flag == "--backend") config.backend = selectBackend(argv[i + 1]);
        else if (flag == "--input") config.input = argv[i + 1];
        else if (flag == "--rate") config.sampleRate = streamRate(argv[i + 1]);
        else if (flag == "--stats-ms") options.statsInterval =
            std::chrono::milliseconds(number(argv[i + 1], 60000, 100));
        else config.output = argv[i + 1];
        i += 2;
    }
    if (!config.outputChannels && capture && !config.output.empty())
        throw std::invalid_argument("--output requires duplex mode");
    return options;
}

void sendInput(const sockaddr_in& address, std::uint64_t frames, const std::optional<sockaddr_in>& local,
               const LiveOptions& options) {
    installStopHandler();
    UdpSocket socket;
    connectSender(socket, address, local);
    Capture capture(frames, options.device.inputChannels, options.device.sampleRate);
    audio::Device device;
    const auto format = device.open(options.device, Capture::callback, &capture);
    if (!device.isOpen() || format.sampleRate != options.device.sampleRate ||
        format.inputChannels != options.device.inputChannels ||
        format.outputChannels != options.device.outputChannels)
        throw std::runtime_error("Could not open the input at the requested sample rate and channel count");
    if (!device.start()) throw std::runtime_error("Audio capture could not start: " + device.error());

    StatsTicker ticker(options.statsInterval);
    std::uint64_t sentPackets = 0, droppedPackets = 0;
    const auto transmit = [&](std::span<const std::byte> packet) {
        if (socket.send(packet, frames == 0)) ++sentPackets;
        else ++droppedPackets;
    };
    auto lastProgress = Clock::now();
    std::uint64_t lastCallbacks = 0;
    while (!stopRequested.load(std::memory_order_relaxed)) {
        if (ticker.due()) {
            const auto stats = device.stats();
            std::cout << "Stats {\"mode\":\"send-input\",\"sample_rate_hz\":" << options.device.sampleRate
                      << ",\"sent_packets\":" << sentPackets
                      << ",\"udp_send_drops\":" << droppedPackets
                      << ",\"missing_input_callbacks\":" << stats.missingInputCallbacks;
            appendDeviceStats(std::cout, stats);
            std::cout << "}\n" << std::flush;
        }
        const auto packet = capture.sender.takePacket();
        if (!packet.empty()) {
            transmit(packet);
            continue;
        }
        if (capture.progress.finished.load(std::memory_order_acquire)) break;

        const auto stats = device.stats();
        if (!stats.running || stats.interrupted) throw std::runtime_error("Audio capture interrupted");
        const auto now = Clock::now();
        if (stats.callbacks != lastCallbacks) {
            lastProgress = now;
            lastCallbacks = stats.callbacks;
        } else if (now - lastProgress >= idleTimeout) {
            throw std::runtime_error("Audio input callback timed out");
        }
        std::this_thread::sleep_for(1ms);
    }
    device.stop();
    for (auto packet = capture.sender.takePacket(); !packet.empty(); packet = capture.sender.takePacket())
        transmit(packet);
    std::cout << "Captured " << capture.progress.processed << " input frames. Missing input: " << capture.missingFrames
              << " frames. Sender queue dropped: " << capture.droppedFrames << " frames.\n";
    if (!frames) std::cout << "UDP send drops: " << droppedPackets << " packets.\n";
}
#endif

void sendWave(const sockaddr_in& address, const File& source, const std::optional<sockaddr_in>& local) {
    auto reader = audio::openWaveStream(source.createInputStream());
    if (!reader || reader->info().channels != 2 || reader->info().frames <= 0)
        throw std::runtime_error("Choose a nonempty stereo WAV");
    const double fileRate = reader->info().sampleRate;
    if (!(fileRate >= minStreamRate && fileRate <= maxStreamRate) || std::floor(fileRate) != fileRate)
        throw std::runtime_error("The WAV rate must be a whole number from 8000 to 96000 Hz");
    const auto sampleRate = static_cast<unsigned>(fileRate);
    if (reader->info().frames > maxStreamFrames(sampleRate))
        throw std::runtime_error("Choose a stereo WAV up to 24 hours long");
    UdpSocket socket;
    connectSender(socket, address, local);
    Sender<2, blockFrames> sender(sampleRate, 0);
    std::array<float, blockFrames> left{}, right{};
    float* channels[]{left.data(), right.data()};
    std::array<Stereo, blockFrames> frames{};
    const auto anchor = Clock::now();
    for (std::int64_t offset = 0; offset < reader->info().frames;) {
        const auto count = static_cast<int>(std::min<std::int64_t>(blockFrames, reader->info().frames - offset));
        if (!reader->read(channels, 2, offset, count)) throw std::runtime_error("WAV read failed");
        for (int i = 0; i < count; ++i) frames[i] = {left[i], right[i]};
        if (!sender.capture(std::span(frames).first(count))) throw std::runtime_error("Sender queue full");
        const auto packet = sender.takePacket();
        std::this_thread::sleep_until(anchor + frameTime(offset, sampleRate));
        if (packet.empty()) throw std::runtime_error("Could not encode WAV packet");
        socket.send(packet);
        offset += count;
    }
    std::cout << "Sent " << reader->info().frames << " frames.\n";
}

struct StreamTotals {
    std::uint64_t outputFrames{};
    std::size_t sessions{};
    std::size_t acceptedPackets{};
    std::size_t concealedFrames{};
    std::size_t unfilledFrames{};
    std::size_t latePackets{};
    std::size_t unexpectedPackets{};
};

// Returns true after an idle continuous session so the socket and output can be reused.
bool receiveSession(UdpSocket& socket, StereoReceiver& receiver, std::uint64_t frames,
                    const File& destination, unsigned sampleRate,
                    std::chrono::milliseconds budget, const std::optional<sockaddr_in>& expectedPeer,
                    Monitor* monitor, StreamTotals& totals) {
    const bool live = monitor != nullptr;
    const bool continuous = live && !frames;
    const auto temporary = live ? File{} : destination.getSiblingFile(".cell-network-" + Uuid().toString());
    struct Cleanup {
        const File& path;
        bool active;
        ~Cleanup() { if (active) path.deleteFile(); }
    } cleanup{temporary, !live};
    std::unique_ptr<audio::AudioFileWriter> writer;
    if (!live) {
        writer = audio::createWaveFile(temporary, {static_cast<double>(sampleRate), 2, 32, true});
        if (!writer || !writer->status()) throw std::runtime_error("Could not open WAV output");
    }

#ifdef CELL_NETWORK_AUDIO_DEVICE
    bool started = false;
    StatsTicker ticker(live ? monitor->statsInterval : 0ms);
#endif
    // Receive full IPv4 datagrams so oversized payloads reach the decoder intact.
    std::array<std::byte, 65536> packet{};
    std::array<Stereo, blockFrames> output{};
    std::array<float, blockFrames> left{}, right{};
    const float* channels[]{left.data(), right.data()};
    std::optional<Clock::time_point> anchor;
    Clock::time_point lastAccepted{};
    auto selectedPeer = expectedPeer;
    std::size_t unexpectedPackets = 0;
    const auto startupTimeout = Clock::now() + 10s;
    const auto finalDeadline = frames
        ? budget + std::chrono::ceil<std::chrono::nanoseconds>(
            std::chrono::duration<double>(static_cast<double>(frames) / sampleRate))
        : std::chrono::nanoseconds::max();
    std::cout << "Listening on port " << ntohs(socket.localAddress().sin_port) << '\n' << std::flush;

    bool senderIdle = false;
    std::uint64_t offset = 0;
    while (live || offset < frames) {
#ifdef CELL_NETWORK_AUDIO_DEVICE
        if (live && stopRequested.load(std::memory_order_relaxed)) break;
#endif
        const auto receiveStarted = Clock::now();
        // Let packets already waiting in the socket fill gaps before concealment.
        // Bound the batch so a busy peer cannot starve playout or stop handling.
        for (int index = 0; index < maxReceiveBatch; ++index) {
            sockaddr_in peer{};
            const auto size = socket.receive(packet, peer, index == 0 ? 1 : 0);
            if (!size) break;
            if (selectedPeer && (peer.sin_addr.s_addr != selectedPeer->sin_addr.s_addr ||
                                 peer.sin_port != selectedPeer->sin_port)) {
                ++unexpectedPackets;
                continue;
            }
            if (receiver.receive(std::span(packet).first(*size)) != PacketAdmission::accepted) continue;
            lastAccepted = Clock::now();
            if (!anchor) {
                // A continuous monitor filters peers in software so it can
                // accept a new source on this socket after the session ends.
                if (!continuous) socket.connect(peer);
                selectedPeer = peer;
                anchor = lastAccepted;
                std::cout << "Sender connected.\n" << std::flush;
            }
        }

#ifdef CELL_NETWORK_AUDIO_DEVICE
        if (monitor && ticker.due()) {
            const auto stats = receiver.stats();
            const auto packetAge = anchor
                ? std::chrono::duration_cast<std::chrono::milliseconds>(Clock::now() - lastAccepted)
                : 0ms;
            const auto state = started ? "playing" : (anchor ? "buffering" : "waiting");
            const auto transportState = !anchor ? "waiting" :
                (packetAge >= packetStallThreshold ? "stalled" : "active");
            const auto queuedFrames = receiver.queuedPlaybackFrames();
            std::cout << "Stats {\"mode\":\"monitor\",\"state\":\"" << state << '"'
                      << ",\"transport_state\":\"" << transportState << '"'
                      << ",\"session\":" << totals.sessions + 1 << ",\"peer\":";
            appendPeer(std::cout, anchor ? &*selectedPeer : nullptr);
            std::cout << ",\"last_accepted_packet_age_ms\":";
            if (anchor) std::cout << packetAge.count();
            else std::cout << "null";
            std::cout << ",\"sample_rate_hz\":" << sampleRate
                      << ",\"arrival_budget_ms\":" << budget.count()
                      << ",\"accepted_packets\":" << stats.acceptedPackets
                      << ",\"invalid_packets\":" << stats.invalidPackets
                      << ",\"late_packets\":" << stats.latePackets
                      << ",\"outside_window_packets\":" << stats.outsideWindowPackets
                      << ",\"overlapping_packets\":" << stats.overlappingPackets
                      << ",\"unexpected_peer_packets\":" << unexpectedPackets
                      << ",\"concealed_source_frames\":" << stats.concealedSourceFrames
                      << ",\"unfilled_output_frames\":" << stats.unfilledOutputFrames
                      << ",\"queued_playback_frames\":" << queuedFrames
                      << ",\"queued_playback_ms\":" << queuedFrames * 1000 / sampleRate;
            appendDeviceStats(std::cout, monitor->device.stats());
            std::cout << "}\n" << std::flush;
        }
#endif
        // Playout may only advance through the receive turn we just serviced.
        // If the process was descheduled after polling, drain the newly queued
        // packets on the next turn before advancing to the current wall clock.
        const auto now = std::max(receiveStarted, lastAccepted);
        if (!anchor) {
            if (!continuous && now >= startupTimeout)
                throw std::runtime_error("No admissible packets within 10 seconds");
            continue;
        }
        if (now - lastAccepted >= idleTimeout) {
            if (!continuous) {
                if (writer) {
                    writer.reset();
                    if (!temporary.deleteFile())
                        throw std::runtime_error("UDP stream timed out and the partial WAV could not be removed");
                    cleanup.active = false;
                }
                throw std::runtime_error("UDP stream timed out before the capture completed");
            }
            senderIdle = true;
            break;
        }
        const auto elapsed = std::chrono::duration_cast<std::chrono::nanoseconds>(now - *anchor);
        while (receiver.service(std::min(elapsed, finalDeadline)).queued != 0) {}

#ifdef CELL_NETWORK_AUDIO_DEVICE
        if (live) {
            if (!started && now >= *anchor + 2 * budget + frameTime(blockFrames, sampleRate)) {
                if (!monitor->device.start())
                    throw std::runtime_error("Audio playback could not start: " + monitor->device.error());
                started = true;
            }
            if (monitor->progress.finished.load(std::memory_order_acquire)) break;
            if (started && monitor->device.stats().interrupted) throw std::runtime_error("Audio playback interrupted");
            continue;
        }
#endif
        const auto count = static_cast<int>(std::min<std::uint64_t>(blockFrames, frames - offset));
        if (now < *anchor + 2 * budget + frameTime(offset + count, sampleRate)) continue;
        receiver.render(std::span(output).first(count));
        for (int i = 0; i < count; ++i) {
            left[i] = output[i][0];
            right[i] = output[i][1];
        }
        if (!writer->write(channels, 2, count)) throw std::runtime_error("WAV write failed");
        offset += count;
    }
#ifdef CELL_NETWORK_AUDIO_DEVICE
    if (monitor) {
        if (started) monitor->device.stop();
        offset = monitor->progress.processed;
        monitor->progress.processed = 0;
        monitor->progress.finished.store(false, std::memory_order_relaxed);
    }
#endif
    if (writer) {
        if (!writer->finish()) throw std::runtime_error("WAV finalization failed");
        writer.reset();
        if (!files::publish(temporary.nativePath(), destination.nativePath(), false))
            throw std::runtime_error("Could not publish WAV output");
    }
    const auto stats = receiver.stats();
    totals.outputFrames += offset;
    ++totals.sessions;
    totals.acceptedPackets += stats.acceptedPackets;
    totals.concealedFrames += stats.concealedSourceFrames;
    totals.unfilledFrames += stats.unfilledOutputFrames;
    totals.latePackets += stats.latePackets;
    totals.unexpectedPackets += unexpectedPackets;
    return senderIdle;
}

void receiveWave(const sockaddr_in& address, std::uint64_t frames, const File& destination, unsigned sampleRate,
                 std::chrono::milliseconds budget, const std::optional<sockaddr_in>& expectedPeer,
                 const LiveOptions* monitorOptions = nullptr) {
    const bool live = monitorOptions != nullptr;
#ifdef CELL_NETWORK_AUDIO_DEVICE
    if (live) installStopHandler();
#endif
    if (!live && !frames) throw std::invalid_argument("WAV capture requires a finite frame count");
    if (!live && destination.exists()) throw std::runtime_error("Choose a new output path");
    UdpSocket socket;
    socket.bind(address);

    // The output must stop before its callback's receiver is replaced or destroyed.
    std::optional<StereoReceiver> receiver;
    Monitor* monitor = nullptr;
#ifdef CELL_NETWORK_AUDIO_DEVICE
    std::optional<Monitor> playback;
    if (live) monitor = &playback.emplace(receiver, frames, *monitorOptions);
#endif
    // Audio can arrive during both the arrival budget and the capture lead.
    const auto prefill = live ? static_cast<std::size_t>(2 * sampleRate * budget.count() / 1000) : 1;
    const ReceiverConfig config{0, sampleRate, budget, prefill, live ? 800u : 0u, live && !frames};
    StreamTotals totals;
    while (receiveSession(socket, receiver.emplace(config), frames, destination, sampleRate, budget, expectedPeer,
                          monitor, totals))
        std::cout << "Sender idle. Waiting for a new stream.\n" << std::flush;

    std::cout << (live ? "Monitored " : "Captured ") << totals.outputFrames << " frames from "
              << totals.acceptedPackets << " packets. Concealed " << totals.concealedFrames
              << " source frames, unfilled " << totals.unfilledFrames << " output frames.\n"
              << "Late packets: " << totals.latePackets << ".\n"
              << "Unexpected peer packets: " << totals.unexpectedPackets << ".\n";
}
void measureLatencySeries(const File& capture, int maxDelayFrames,
                          std::span<const std::int64_t> references) {
    using namespace cell::network_audio::example;
    const auto reader = openLatencyCapture(capture, defaultRate);
    std::vector<LatencyMeasurement> measurements;
    std::vector<std::size_t> delays;
    for (const auto frame : references) {
        try {
            const auto measurement = measureLatency(*reader, frame, maxDelayFrames);
            measurements.push_back(measurement);
            delays.push_back(measurement.delayFrames);
        } catch (const std::exception& error) {
            throw std::runtime_error("Probe at frame " + std::to_string(frame) + " failed: " + error.what());
        }
    }

    const auto milliseconds = [](double samples) { return 1000 * samples / defaultRate; };
    std::cout << std::fixed << std::setprecision(3);
    for (const auto& measurement : measurements) {
        std::cout << "Reference " << measurement.referenceFrame << ": " << measurement.delayFrames << " frames ("
                  << milliseconds(static_cast<double>(measurement.delayFrames)) << " ms), correlation "
                  << measurement.correlation << ", "
                  << (measurement.inverted ? "inverted" : "normal") << " polarity.\n";
    }
    std::ranges::sort(delays);
    const auto median = (static_cast<double>(delays[(delays.size() - 1) / 2]) + delays[delays.size() / 2]) / 2;
    std::cout << "Median delay: " << median << " frames (" << milliseconds(median) << " ms).\n"
              << "Minimum: " << delays.front() << " frames. Maximum: " << delays.back()
              << " frames. Spread: " << delays.back() - delays.front() << " frames ("
              << milliseconds(static_cast<double>(delays.back() - delays.front())) << " ms).\n";
}

} // namespace

int main(int argc, char** argv) {
    try {
        const std::string_view mode = argc > 1 ? argv[1] : "";
#ifdef CELL_NETWORK_AUDIO_DEVICE
        if (mode == "devices" && (argc == 2 || argc == 3)) {
            listDevices(argc == 3 ? selectBackend(argv[2]) : audio::Backend::automatic);
            return 0;
        }
        if (mode == "send-input" && argc >= 5) {
            int option = 5;
            std::optional<sockaddr_in> local;
            if (option < argc && !std::string_view(argv[option]).starts_with("--"))
                local = endpoint("0.0.0.0", argv[option++]);
            const auto options = deviceOptions(argc, argv, option, true);
            sendInput(endpoint(argv[2], argv[3]), liveFrameLimit(argv[4], options.device.sampleRate), local, options);
            return 0;
        }
        if (mode == "monitor" && argc >= 5) {
            int option = 5;
            auto budget = defaultBudget;
            if (option < argc && !std::string_view(argv[option]).starts_with("--"))
                budget = std::chrono::milliseconds(number(argv[option++], maxBudget.count(), minBudget.count()));
            const auto options = deviceOptions(argc, argv, option, false);
            receiveWave(endpoint(argv[2], argv[3], true), liveFrameLimit(argv[4], options.device.sampleRate),
                        {}, options.device.sampleRate, budget, options.peer, &options);
            return 0;
        }
#endif
        if (mode == "make-latency-probe" && (argc == 3 || argc == 4)) {
            const auto count = argc == 4 ? static_cast<unsigned>(number(argv[3], 32)) : 1;
            cell::network_audio::example::writeLatencyProbe(cell::File(argv[2]), defaultRate, count);
            std::cout << "Wrote a stereo latency probe. Reference window starts at frame " << defaultRate / 10
                      << " and spans " << cell::network_audio::example::latencyProbeFrames << " frames.\n";
            if (count > 1) std::cout << count << " probes spaced 2.5 seconds apart.\n";
        } else if (mode == "measure-latency-series" && argc >= 6) {
            const auto maxDelayFrames = static_cast<int>(number(argv[3], 2000) * defaultRate / 1000);
            std::vector<std::int64_t> references;
            for (int index = 4; index < argc; ++index) {
                const auto frame = number(argv[index], maxStreamFrames(defaultRate), 0);
                if (!references.empty() && frame <= references.back())
                    throw std::invalid_argument("Reference frames must be strictly increasing");
                references.push_back(frame);
            }
            measureLatencySeries(cell::File(argv[2]), maxDelayFrames, references);
        } else if (mode == "measure-latency" && argc == 5) {
            std::optional<std::int64_t> referenceFrame;
            if (std::string_view(argv[3]) != "auto") referenceFrame = number(argv[3], maxStreamFrames(defaultRate), 0);
            const auto maxDelayMs = number(argv[4], 2000);
            const auto reader = cell::network_audio::example::openLatencyCapture(cell::File(argv[2]), defaultRate);
            const auto measurement = cell::network_audio::example::measureLatency(
                *reader, referenceFrame, static_cast<int>(maxDelayMs * defaultRate / 1000));
            if (!referenceFrame) std::cout << "Reference frame: " << measurement.referenceFrame << '\n';
            std::cout << "Delay: " << measurement.delayFrames << " frames (" << std::fixed
                      << std::setprecision(3) << 1000.0 * measurement.delayFrames / defaultRate << " ms)\n"
                      << "Correlation: " << measurement.correlation << '\n'
                      << "Polarity: " << (measurement.inverted ? "inverted" : "normal") << '\n';
        } else if (mode == "send" && (argc == 5 || argc == 6)) {
            const auto local = argc == 6 ? std::optional(endpoint("0.0.0.0", argv[5])) : std::nullopt;
            sendWave(endpoint(argv[2], argv[3]), cell::File(argv[4]), local);
        } else if (mode == "receive" && argc >= 6) {
            auto budget = defaultBudget;
            auto sampleRate = defaultRate;
            int option = 6;
            if (option < argc && !std::string_view(argv[option]).starts_with("--"))
                budget = std::chrono::milliseconds(number(argv[option++], maxBudget.count(), minBudget.count()));
            std::optional<sockaddr_in> peer;
            while (option < argc) {
                const std::string_view flag = argv[option];
                if (flag == "--peer" && !peer) {
                    peer = peerOption(argc, argv, option);
                    option += 3;
                } else if (flag == "--rate" && option + 1 < argc) {
                    sampleRate = streamRate(argv[option + 1]);
                    option += 2;
                } else {
                    throw std::invalid_argument("Expected --peer IPV4 PORT or --rate HZ");
                }
            }
            receiveWave(endpoint(argv[2], argv[3], true), number(argv[4], maxStreamFrames(sampleRate)),
                        cell::File(argv[5]), sampleRate, budget, peer);
        } else {
            throw std::invalid_argument(
#ifdef CELL_NETWORK_AUDIO_DEVICE
                "Usage: cell_network_audio_wave devices [BACKEND]\n"
                "       cell_network_audio_wave monitor BIND_IPV4 PORT FRAMES|continuous [ARRIVAL_BUDGET_MS]"
                " [--backend BACKEND] [--output ID] [--peer IPV4 PORT] [--rate HZ] [--stats-ms MS]\n"
                "       cell_network_audio_wave send-input DEST_IPV4 PORT FRAMES|continuous [SOURCE_PORT]"
                " [--backend BACKEND] [--input ID] [--mono] [--capture-only|--output ID] [--rate HZ] [--stats-ms MS]\n"
                "       cell_network_audio_wave send DEST_IPV4 PORT INPUT.wav [SOURCE_PORT]\n"
#else
                "Usage: cell_network_audio_wave send DEST_IPV4 PORT INPUT.wav [SOURCE_PORT]\n"
#endif
                "       cell_network_audio_wave receive BIND_IPV4 PORT FRAMES OUTPUT.wav "
                "[ARRIVAL_BUDGET_MS] [--peer IPV4 PORT] [--rate HZ]\n"
                "       cell_network_audio_wave make-latency-probe OUTPUT.wav [COUNT]\n"
                "       cell_network_audio_wave measure-latency CAPTURE.wav REFERENCE_FRAME|auto MAX_DELAY_MS\n"
                "       cell_network_audio_wave measure-latency-series CAPTURE.wav MAX_DELAY_MS FRAME FRAME [FRAME ...]");
        }
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
