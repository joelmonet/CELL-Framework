#pragma once
#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace cell::audio {
enum class Backend {automatic,pulse,alsa,jack,wasapi,coreAudio,aAudio,openSL};
const char* backendName(Backend)noexcept;
struct DeviceInfo {std::string id,name;bool defaultDevice{};};
struct DeviceList {Backend backend{};std::vector<DeviceInfo> inputs,outputs;};
enum class AAudioPlaybackMode { standard, lowLatency, oneBurst };
struct DeviceConfig {
    Backend backend=Backend::automatic;
    std::string input,output; // empty selects the backend's default endpoint
    unsigned sampleRate=48000,periodFrames=128,periods=2,inputChannels=2,outputChannels=2;
    // Zero input channels selects playback only. Zero output channels selects
    // capture only. A zero sample rate chooses the device rate.
    bool allowSampleRateConversion=false;
    // Playback-only AAudio streams can prefer exclusive sharing and target two
    // native bursts. The one-burst option requires a granted exclusive low-latency stream.
    AAudioPlaybackMode aAudioPlaybackMode=AAudioPlaybackMode::standard;
};
struct AAudioPlaybackInfo {
    int sampleRate{},deviceId{},framesPerBurst{},bufferFrames{},capacityFrames{},xruns{-1};
    bool exclusive{},lowLatency{};
};
struct DeviceFormat {
    Backend backend{};
    unsigned sampleRate{},inputChannels{},outputChannels{},requestedPeriodFrames{};
    unsigned capturePeriodFrames{},capturePeriods{},playbackPeriodFrames{},playbackPeriods{};
    // Backend buffer sizes are an estimate, not a measured analogue/device delay.
    bool exactHardwareLatencyKnown=false;
    unsigned captureBridgeFrames{},captureBridgeCapacityFrames{};
    std::uint64_t inputBufferEstimate()const noexcept{return std::uint64_t(capturePeriodFrames)*capturePeriods+captureBridgeFrames;}
    std::uint64_t outputBufferEstimate()const noexcept{return std::uint64_t(playbackPeriodFrames)*playbackPeriods;}
};
struct DeviceStats {std::uint64_t callbacks{},frames{},missingInputCallbacks{},callbackOverruns{},maximumCallbackNanoseconds{};bool running{},interrupted{};std::uint64_t captureUnderrunFrames{},captureOverrunFrames{},capturePrerollDiscardedFrames{},serverPlaybackUnderruns{},serverCaptureOverruns{},startupPlaybackUnderruns{},startupCaptureOverruns{},serverPlaybackOverruns{},startupPlaybackOverruns{};};
// Interleaved float32, with the exact negotiated channel counts. The callback
// owns every output sample, must not throw, allocate, block, or call lifecycle
// methods. Output is null for capture-only streams. Input is null when capture
// is disabled or the backend has no samples for this callback.
using Callback=void(*)(void* owner,float* output,const float* input,unsigned frames)noexcept;
// Optional backend implementation. Construction, enumeration/open/close and
// start/stop belong to one non-audio thread. stop() joins the active callback.
// No dummy/null backend and no implicit device start are provided.
class Device {
public:
    Device();~Device();Device(const Device&)=delete;Device& operator=(const Device&)=delete;
    DeviceList enumerate(Backend=Backend::automatic);
    DeviceFormat open(const DeviceConfig&,Callback,void* owner);
    bool start();void stop()noexcept;void close()noexcept;
    bool isOpen()const noexcept;
    DeviceFormat format()const;DeviceStats stats()const noexcept;
    // Read the active AAudio stream on the lifecycle thread. Buffer frames
    // describe the software queue threshold. Hardware latency needs a loopback measurement.
    std::optional<AAudioPlaybackInfo> aAudioPlaybackInfo() const noexcept;
    std::string error()const; // lifecycle thread only
private:struct Impl;std::unique_ptr<Impl> impl_;
};
}
