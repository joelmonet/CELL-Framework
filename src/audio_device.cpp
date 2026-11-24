#include <cell/audio_device.hpp>
#include <cell/realtime.hpp>
#include <atomic>
#include <chrono>
#include <cstring>
#include <stdexcept>
#include <algorithm>
#include <array>
#include <limits>
#include <span>
#include <thread>
// Private linkage isolates the optional I/O backend from decoder-only clients
// that embed another configuration of miniaudio. No global ma_* API is exported.
#if defined(__GNUC__) || defined(__clang__)
#define MA_API static __attribute__((unused))
#else
#define MA_API static
#endif
#define MA_NO_DECODING
#define MA_NO_ENCODING
#define MA_NO_ENGINE
#define MA_NO_RESOURCE_MANAGER
#define MA_NO_NODE_GRAPH
#define MA_NO_GENERATION
#define MA_NO_NULL
#define ma_atomic_global_lock cell_device_backend_atomic_lock
#define MINIAUDIO_IMPLEMENTATION
#include <miniaudio.h>
#if defined(MA_SUPPORT_AAUDIO) && !defined(MA_NO_RUNTIME_LINKING)
#include <aaudio/AAudio.h>
#endif
static_assert(MA_VERSION_MAJOR==0&&MA_VERSION_MINOR==11&&MA_VERSION_REVISION==23,"Review and qualify Cell's private device integration before updating miniaudio");

namespace cell::audio {
namespace {
#if defined(MA_SUPPORT_PULSEAUDIO)
// This service exposes numbered device wires. Pulse's default semantic remix
// can otherwise reorder surround channels or synthesize an LFE signal. Keep
// the separately licensed backend unchanged and wrap only our private context's
// connection entry points. These functions run during device opening, never in
// the audio callback. Atomic originals also allow independent lifecycle threads.
std::atomic<ma_pa_stream_connect_playback_proc> pulsePlayback{};
std::atomic<ma_pa_stream_connect_record_proc> pulseCapture{};
std::atomic<ma_pa_stream_new_proc> pulseNewStream{};
ma_pa_stream* newPulseStream(ma_pa_context* context,const char* name,const ma_pa_sample_spec* format,const ma_pa_channel_map* map){
    auto nativeFormat=*format;
    nativeFormat.format=ma_is_little_endian()?MA_PA_SAMPLE_FLOAT32LE:MA_PA_SAMPLE_FLOAT32BE;
    return pulseNewStream.load(std::memory_order_acquire)(context,name,&nativeFormat,map);
}
int connectPlayback(ma_pa_stream* stream,const char* device,const ma_pa_buffer_attr* attr,ma_pa_stream_flags_t flags,const ma_pa_cvolume* volume,ma_pa_stream* sync){
    flags=ma_pa_stream_flags_t(flags|MA_PA_STREAM_NO_REMAP_CHANNELS|MA_PA_STREAM_NO_REMIX_CHANNELS);
    auto buffer=*attr;const auto period=buffer.tlength;
    buffer.tlength=buffer.maxlength;buffer.minreq=period;buffer.prebuf=0;
    buffer.maxlength=std::max(buffer.maxlength,ma_uint32(std::min<std::uint64_t>(UINT32_MAX,std::uint64_t(period)*32)));
    return pulsePlayback.load(std::memory_order_acquire)(stream,device,&buffer,flags,volume,sync);
}
int connectCapture(ma_pa_stream* stream,const char* device,const ma_pa_buffer_attr* attr,ma_pa_stream_flags_t flags){
    flags=ma_pa_stream_flags_t(flags|MA_PA_STREAM_NO_REMAP_CHANNELS|MA_PA_STREAM_NO_REMIX_CHANNELS);
    auto buffer=*attr;buffer.maxlength=std::max(buffer.maxlength,ma_uint32(std::min<std::uint64_t>(UINT32_MAX,std::uint64_t(buffer.fragsize)*32)));
    return pulseCapture.load(std::memory_order_acquire)(stream,device,&buffer,flags);
}
#endif
ma_backend native(Backend b){switch(b){case Backend::pulse:return ma_backend_pulseaudio;case Backend::alsa:return ma_backend_alsa;case Backend::jack:return ma_backend_jack;case Backend::wasapi:return ma_backend_wasapi;case Backend::coreAudio:return ma_backend_coreaudio;case Backend::aAudio:return ma_backend_aaudio;case Backend::openSL:return ma_backend_opensl;default:return ma_backend_null;}}
Backend portable(ma_backend b){switch(b){case ma_backend_pulseaudio:return Backend::pulse;case ma_backend_alsa:return Backend::alsa;case ma_backend_jack:return Backend::jack;case ma_backend_wasapi:return Backend::wasapi;case ma_backend_coreaudio:return Backend::coreAudio;case ma_backend_aaudio:return Backend::aAudio;case ma_backend_opensl:return Backend::openSL;default:throw std::runtime_error("Unsupported audio backend; no dummy fallback is allowed");}}
std::string id(const ma_device_id& value,ma_backend backend){
    // Encode only the active union member, never uninitialized ID padding.
    const auto string=[](const auto& chars){const auto end=std::find(std::begin(chars),std::end(chars),0);return std::string(std::begin(chars),end);};
    switch(backend){case ma_backend_pulseaudio:return string(value.pulse);case ma_backend_alsa:return string(value.alsa);case ma_backend_coreaudio:return string(value.coreaudio);case ma_backend_jack:return std::to_string(value.jack);
        case ma_backend_aaudio:return std::to_string(value.aaudio);
        case ma_backend_opensl:return std::to_string(value.opensl);
        case ma_backend_wasapi:{constexpr char hex[]="0123456789abcdef";std::string result;for(auto character:value.wasapi){if(!character)break;for(int bit=12;bit>=0;bit-=4)result+=hex[(unsigned(character)>>bit)&15];}return result;}
        default:throw std::runtime_error("Device identifiers for this backend are unavailable");}
}
}
const char* backendName(Backend b)noexcept{switch(b){case Backend::pulse:return "PulseAudio";case Backend::alsa:return "ALSA";case Backend::jack:return "JACK";case Backend::wasapi:return "WASAPI";case Backend::coreAudio:return "CoreAudio";case Backend::aAudio:return "AAudio";case Backend::openSL:return "OpenSL ES";default:return "Automatic";}}
struct Device::Impl {
    ma_context context{};ma_device device{};bool contextReady{},deviceReady{};Backend requested=Backend::automatic;
    std::vector<ma_device_info> capture,playback;DeviceFormat negotiated;Callback callback{};void* owner{};std::string message;
    std::atomic<std::uint64_t> callbacks{},frames{},missing{},overruns{},maximum{},captureUnderruns{},captureOverruns{},capturePreroll{};
    std::atomic<std::uint64_t> serverPlaybackUnderruns{},serverCaptureOverruns{},startupPlaybackUnderruns{},startupCaptureOverruns{},serverPlaybackOverruns{},startupPlaybackOverruns{};
    std::atomic<bool> running{},interrupted{};
    std::vector<float> pulseInput;unsigned pulseQuantum{};bool pulsePriming{};std::atomic<bool> pulsePlaybackStarted{};
#if defined(MA_SUPPORT_AAUDIO) && !defined(MA_NO_RUNTIME_LINKING)
    std::optional<AAudioPlaybackInfo> readAAudioPlayback(AAudioPlaybackMode mode) noexcept {
        if (!deviceReady || context.backend != ma_backend_aaudio || !negotiated.outputChannels)
            return std::nullopt;
        const auto symbol = [&](auto signature, const char* name) {
            return reinterpret_cast<decltype(signature)>(ma_dlsym(ma_context_get_log(&context), context.aaudio.hAAudio, name));
        };
        const auto getBuffer = symbol(decltype(&AAudioStream_getBufferSizeInFrames){}, "AAudioStream_getBufferSizeInFrames");
        const auto setBuffer = symbol(decltype(&AAudioStream_setBufferSizeInFrames){}, "AAudioStream_setBufferSizeInFrames");
        const auto getSharing = symbol(decltype(&AAudioStream_getSharingMode){}, "AAudioStream_getSharingMode");
        const auto getPerformance = symbol(decltype(&AAudioStream_getPerformanceMode){}, "AAudioStream_getPerformanceMode");
        const auto getXRuns = symbol(decltype(&AAudioStream_getXRunCount){}, "AAudioStream_getXRunCount");
        const auto getDevice = symbol(decltype(&AAudioStream_getDeviceId){}, "AAudioStream_getDeviceId");
        if (!getBuffer || !getSharing || !getPerformance || !context.aaudio.AAudioStream_getFramesPerBurst)
            return std::nullopt;

        std::optional<AAudioPlaybackInfo> result;
        // miniaudio can replace this stream on its AAudio job thread.
        ma_mutex_lock(&device.aaudio.rerouteLock);
        if (auto* stream = static_cast<AAudioStream*>(device.aaudio.pStreamPlayback)) {
            auto* backendStream = reinterpret_cast<ma_AAudioStream*>(stream);
            AAudioPlaybackInfo info;
            info.sampleRate = reinterpret_cast<MA_PFN_AAudioStream_getSampleRate>(
                context.aaudio.AAudioStream_getSampleRate)(backendStream);
            info.deviceId = getDevice ? getDevice(stream) : 0;
            info.framesPerBurst = reinterpret_cast<MA_PFN_AAudioStream_getFramesPerBurst>(
                context.aaudio.AAudioStream_getFramesPerBurst)(backendStream);
            info.capacityFrames = reinterpret_cast<MA_PFN_AAudioStream_getBufferCapacityInFrames>(
                context.aaudio.AAudioStream_getBufferCapacityInFrames)(backendStream);
            info.bufferFrames = getBuffer(stream);
            info.exclusive = getSharing(stream) == AAUDIO_SHARING_MODE_EXCLUSIVE;
            info.lowLatency = getPerformance(stream) == AAUDIO_PERFORMANCE_MODE_LOW_LATENCY;
            info.xruns = getXRuns ? getXRuns(stream) : -1;
            if (mode != AAudioPlaybackMode::standard && setBuffer && info.lowLatency &&
                info.framesPerBurst > 0 && info.framesPerBurst <= info.capacityFrames) {
                const auto bursts = mode == AAudioPlaybackMode::oneBurst && info.exclusive ? 1 : 2;
                const auto target = static_cast<int>(std::min<std::int64_t>(
                    info.capacityFrames, std::int64_t(info.framesPerBurst) * bursts));
                if (target < info.bufferFrames) {
                    const auto actual = setBuffer(stream, target);
                    if (actual > 0) info.bufferFrames = actual;
                }
            }
            if (info.sampleRate > 0 && info.framesPerBurst > 0 && info.bufferFrames > 0)
                result = info;
        }
        ma_mutex_unlock(&device.aaudio.rerouteLock);
        return result;
    }
#endif
#if defined(MA_SUPPORT_PULSEAUDIO)
    using NotifySetter=void(*)(ma_pa_stream*,ma_pa_stream_notify_cb_t,void*);
    static void pulseUnderflow(ma_pa_stream*,void* owner)noexcept{auto& p=*static_cast<Impl*>(owner);(p.running.load(std::memory_order_acquire)?p.serverPlaybackUnderruns:p.startupPlaybackUnderruns).fetch_add(1,std::memory_order_relaxed);}
    static void pulseOverflow(ma_pa_stream*,void* owner)noexcept{auto& p=*static_cast<Impl*>(owner);(p.running.load(std::memory_order_acquire)?p.serverCaptureOverruns:p.startupCaptureOverruns).fetch_add(1,std::memory_order_relaxed);}
    static void pulsePlaybackOverflow(ma_pa_stream*,void* owner)noexcept{auto& p=*static_cast<Impl*>(owner);(p.running.load(std::memory_order_acquire)?p.serverPlaybackOverruns:p.startupPlaybackOverruns).fetch_add(1,std::memory_order_relaxed);}
    static bool pulseActive(ma_device& device)noexcept{const auto state=ma_device_get_state(&device);return state==ma_device_state_started||state==ma_device_state_starting;}
    static void discardPreroll(Impl& p,unsigned count)noexcept{
        auto& ring=p.device.duplexRB.rb;
        while(count){ma_uint32 amount=count;void* ignored{};if(ma_pcm_rb_acquire_read(&ring,&amount,&ignored)!=MA_SUCCESS||!amount)break;ma_pcm_rb_commit_read(&ring,amount);p.capturePreroll.fetch_add(amount,std::memory_order_relaxed);count-=amount;}
    }
    static void pulseRead(ma_pa_stream* stream,size_t,void* owner)noexcept{
        auto& p=*static_cast<Impl*>(owner);if(!pulseActive(p.device))return;
        auto& api=p.context.pulse;auto& ring=p.device.duplexRB.rb;
        for(;;){
            const void* samples{};size_t bytes{};
            if(reinterpret_cast<ma_pa_stream_peek_proc>(api.pa_stream_peek)(stream,&samples,&bytes)<0||!bytes)break;
            const auto channels=p.device.capture.channels;const auto count=bytes/(channels*sizeof(float));std::size_t done=0;
            if(!p.pulsePlaybackStarted.load(std::memory_order_relaxed)){
                // A server may start its playback clock later than capture.
                // Before the first DSP sample, retain fresh priming data only.
                // Intentional startup trimming is distinct from runtime loss.
                const auto retain=p.negotiated.captureBridgeFrames+2*p.pulseQuantum;
                const auto queued=ma_pcm_rb_available_read(&ring);
                if(count>=retain){discardPreroll(p,queued);done=count-retain;p.capturePreroll.fetch_add(done,std::memory_order_relaxed);}
                else if(queued+count>retain)discardPreroll(p,unsigned(queued+count-retain));
            }
            while(done<count){
                ma_uint32 amount=ma_uint32(std::min<std::size_t>(count-done,UINT32_MAX));void* target{};
                if(ma_pcm_rb_acquire_write(&ring,&amount,&target)!=MA_SUCCESS||!amount){p.captureOverruns.fetch_add(count-done,std::memory_order_relaxed);break;}
                if(samples)std::copy_n(static_cast<const float*>(samples)+done*channels,std::size_t(amount)*channels,static_cast<float*>(target));
                else {std::fill_n(static_cast<float*>(target),std::size_t(amount)*channels,0.f);p.captureUnderruns.fetch_add(amount,std::memory_order_relaxed);}
                ma_pcm_rb_commit_write(&ring,amount);done+=amount;
            }
            if(reinterpret_cast<ma_pa_stream_drop_proc>(api.pa_stream_drop)(stream)<0)break;
        }
    }
    static void pulseWrite(ma_pa_stream* stream,size_t,void* owner)noexcept{
        auto& p=*static_cast<Impl*>(owner);if(p.pulsePriming)return;auto& api=p.context.pulse;auto& ring=p.device.duplexRB.rb;
        const auto inputChannels=p.device.capture.channels,outputChannels=p.device.playback.channels;
        const auto stride=std::size_t(outputChannels)*sizeof(float);
        // The notification's byte hint can be stale after event dispatch. Fill
        // the live writable region; leaving part of it empty can starve PA even
        // when every DSP callback meets its deadline. Bound catch-up work.
        for(unsigned iteration=0;iteration<256&&pulseActive(p.device);++iteration){
            const auto writable=reinterpret_cast<ma_pa_stream_writable_size_proc>(api.pa_stream_writable_size)(stream);
            if(writable==size_t(-1)){p.interrupted.store(true,std::memory_order_release);break;}
            const auto requested=std::min(writable,std::size_t(p.pulseQuantum)*stride);if(requested<stride)break;
            size_t bytes=requested;void* output{};
            if(reinterpret_cast<ma_pa_stream_begin_write_proc>(api.pa_stream_begin_write)(stream,&output,&bytes)<0||!bytes){p.interrupted.store(true,std::memory_order_release);break;}
            bytes=std::min(bytes,requested); // PA may reserve a larger memory block.
            const auto count=unsigned(bytes/stride);unsigned copied=0;
            const bool began=p.pulsePlaybackStarted.load(std::memory_order_relaxed);
            const bool capture=p.negotiated.inputChannels!=0;
            const auto queued=capture?ma_pcm_rb_available_read(&ring):0u;
            const bool deliver=ma_device_get_state(&p.device)==ma_device_state_started&&(!capture||began||std::uint64_t(queued)>=std::uint64_t(p.negotiated.captureBridgeFrames)+count);
            if(capture&&deliver&&!began)discardPreroll(p,queued-p.negotiated.captureBridgeFrames-count);
            while(capture&&deliver&&copied<count){
                ma_uint32 amount=count-copied;void* input{};
                if(ma_pcm_rb_acquire_read(&ring,&amount,&input)!=MA_SUCCESS||!amount)break;
                std::copy_n(static_cast<const float*>(input),std::size_t(amount)*inputChannels,p.pulseInput.data()+std::size_t(copied)*inputChannels);
                ma_pcm_rb_commit_read(&ring,amount);copied+=amount;
            }
            if(capture&&deliver&&copied<count){std::fill(p.pulseInput.begin()+std::size_t(copied)*inputChannels,p.pulseInput.begin()+std::size_t(count)*inputChannels,0.f);p.captureUnderruns.fetch_add(count-copied,std::memory_order_relaxed);}
            // Advance every playback sample, including a missing capture period.
            // No repeated/stale output and no stopped DSP clock on underflow.
            if(deliver){data(&p.device,output,copied?p.pulseInput.data():nullptr,count);p.pulsePlaybackStarted.store(true,std::memory_order_release);}
            else std::memset(output,0,bytes);
            if(reinterpret_cast<ma_pa_stream_write_proc>(api.pa_stream_write)(stream,output,bytes,nullptr,0,MA_PA_SEEK_RELATIVE)<0){p.interrupted.store(true,std::memory_order_release);break;}
        }
    }
    static ma_result startPulse(ma_device* device){
        if(device->type!=ma_device_type_duplex&&device->type!=ma_device_type_playback)return ma_device_start__pulse(device);
        auto& p=*static_cast<Impl*>(device->pUserData);auto& api=p.context.pulse;
        p.pulsePriming=true;p.pulsePlaybackStarted.store(false,std::memory_order_release);
        if(device->type==ma_device_type_duplex){ma_pcm_rb_reset(&device->duplexRB.rb);const auto result=ma_device__cork_stream__pulse(device,ma_device_type_capture,0);if(result!=MA_SUCCESS)return result;}
        auto* stream=static_cast<ma_pa_stream*>(device->pulse.pStreamPlayback);
        const auto bytes=reinterpret_cast<ma_pa_stream_writable_size_proc>(api.pa_stream_writable_size)(stream);
        if(bytes==size_t(-1))return MA_ERROR;
        // Uncork with silence. The ordinary PA loop then primes capture and
        // starts the DSP timeline on the first real playback demand.
        p.pulsePriming=false;pulseWrite(stream,bytes,&p);if(p.interrupted.load(std::memory_order_acquire))return MA_ERROR;
        return ma_device__cork_stream__pulse(device,ma_device_type_playback,0);
    }
#endif
    void initialize(Backend backend){
        if(unsigned(backend)>unsigned(Backend::openSL))throw std::invalid_argument("Unknown audio backend");
        if(contextReady&&requested==backend)return;
        if(deviceReady)throw std::logic_error("Stop and close audio before changing its backend");
        if(contextReady){ma_context_uninit(&context);contextReady=false;}capture.clear();playback.clear();requested=backend;
        const auto selected=native(backend);auto config=ma_context_config_init();config.jack.tryStartServer=MA_FALSE;
        const auto result=ma_context_init(backend==Backend::automatic?nullptr:&selected,backend==Backend::automatic?0:1,&config,&context);
        if(result!=MA_SUCCESS)throw std::runtime_error(std::string("Audio backend initialization failed: ")+ma_result_description(result));
        contextReady=true;(void)portable(context.backend);
#if defined(MA_SUPPORT_PULSEAUDIO)
        if(context.backend==ma_backend_pulseaudio){
            pulsePlayback.store(reinterpret_cast<ma_pa_stream_connect_playback_proc>(context.pulse.pa_stream_connect_playback),std::memory_order_release);
            pulseCapture.store(reinterpret_cast<ma_pa_stream_connect_record_proc>(context.pulse.pa_stream_connect_record),std::memory_order_release);
            pulseNewStream.store(reinterpret_cast<ma_pa_stream_new_proc>(context.pulse.pa_stream_new),std::memory_order_release);
            context.pulse.pa_stream_connect_playback=reinterpret_cast<ma_proc>(connectPlayback);
            context.pulse.pa_stream_connect_record=reinterpret_cast<ma_proc>(connectCapture);
            context.pulse.pa_stream_new=reinterpret_cast<ma_proc>(newPulseStream);
            context.callbacks.onDeviceStart=startPulse;
        }
#endif
    }
    DeviceList enumerate(Backend backend){
        initialize(backend);ma_device_info *out{},*in{};ma_uint32 outputs{},inputs{};
        const auto result=ma_context_get_devices(&context,&out,&outputs,&in,&inputs);
        if(result!=MA_SUCCESS)throw std::runtime_error(std::string("Audio device enumeration failed: ")+ma_result_description(result));
        playback.clear();capture.clear();if(outputs)playback.assign(out,out+outputs);if(inputs)capture.assign(in,in+inputs);DeviceList list;list.backend=portable(context.backend);
        for(const auto& item:capture)list.inputs.push_back({id(item.id,context.backend),item.name,item.isDefault!=0});
        for(const auto& item:playback)list.outputs.push_back({id(item.id,context.backend),item.name,item.isDefault!=0});
        return list;
    }
    static void notification(const ma_device_notification* event){
        auto& self=*static_cast<Impl*>(event->pDevice->pUserData);
        if(event->type==ma_device_notification_type_stopped)self.running.store(false,std::memory_order_release);
        if(event->type==ma_device_notification_type_interruption_began)self.interrupted.store(true,std::memory_order_release);
        if(event->type==ma_device_notification_type_interruption_ended)self.interrupted.store(false,std::memory_order_release);
    }
    static void data(ma_device* device,void* output,const void* input,ma_uint32 count){
        auto& self=*static_cast<Impl*>(device->pUserData);ScopedNoDenormals guard;
        const auto begin=std::chrono::steady_clock::now();
        if(!input&&self.negotiated.inputChannels)self.missing.fetch_add(1,std::memory_order_relaxed);
        if(output||!self.negotiated.outputChannels)self.callback(self.owner,static_cast<float*>(output),static_cast<const float*>(input),count);
        const auto duration=std::uint64_t(std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now()-begin).count());
        auto previous=self.maximum.load(std::memory_order_relaxed);if(duration>previous)self.maximum.store(duration,std::memory_order_relaxed);
        if(duration>std::uint64_t(count)*1000000000ull/device->sampleRate)self.overruns.fetch_add(1,std::memory_order_relaxed);
        self.frames.fetch_add(count,std::memory_order_relaxed);self.callbacks.fetch_add(1,std::memory_order_release);
    }
    void close()noexcept{if(deviceReady){ma_device_uninit(&device);deviceReady=false;}running.store(false,std::memory_order_release);callback=nullptr;owner=nullptr;}
    ~Impl(){close();if(contextReady)ma_context_uninit(&context);}
};
Device::Device():impl_(std::make_unique<Impl>()){}Device::~Device()=default;
DeviceList Device::enumerate(Backend backend){return impl_->enumerate(backend);}
DeviceFormat Device::open(const DeviceConfig& config,Callback callback,void* owner){
    const bool capture=config.inputChannels!=0, playback=config.outputChannels!=0;
    if(!callback||!config.periodFrames||!config.periods||(!capture&&!playback)||
       config.inputChannels>MA_MAX_CHANNELS||config.outputChannels>MA_MAX_CHANNELS)
        throw std::invalid_argument("Invalid device dimensions for the selected native backend");
    auto& p=*impl_;if(p.deviceReady)throw std::logic_error("Close the active device before opening another");p.message.clear();p.enumerate(config.backend);
    auto select=[&](const std::vector<ma_device_info>& values,const std::string& key)->const ma_device_id*{if(key.empty())return nullptr;for(const auto& value:values)if(id(value.id,p.context.backend)==key)return &value.id;throw std::invalid_argument("Selected audio endpoint is no longer available");};
    auto type=ma_device_type_playback;
    if(capture)type=playback?ma_device_type_duplex:ma_device_type_capture;
    auto settings=ma_device_config_init(type);settings.sampleRate=config.sampleRate;settings.periodSizeInFrames=config.periodFrames;settings.periods=config.periods;settings.performanceProfile=ma_performance_profile_low_latency;
    settings.capture.format=settings.playback.format=ma_format_f32;settings.capture.channels=config.inputChannels;settings.playback.channels=config.outputChannels;
    settings.capture.pDeviceID=capture?select(p.capture,config.input):nullptr;
    settings.playback.pDeviceID=playback?select(p.playback,config.output):nullptr;
    const auto capacity=[&](ma_device_type type,const ma_device_id* selected,unsigned channels){
        ma_device_info info{};if(ma_context_get_device_info(&p.context,type,selected,&info)!=MA_SUCCESS)return;
        unsigned known=0;for(unsigned i=0;i<info.nativeDataFormatCount;++i){if(!info.nativeDataFormats[i].channels)return;known=std::max(known,info.nativeDataFormats[i].channels);}
        if(known&&channels>known)throw std::invalid_argument(type==ma_device_type_playback?"The selected output endpoint has too few native channels for this speaker map":"The selected capture endpoint has too few native channels");
    };
    if(capture)capacity(ma_device_type_capture,settings.capture.pDeviceID,config.inputChannels);
    if(playback)capacity(ma_device_type_playback,settings.playback.pDeviceID,config.outputChannels);
    settings.capture.channelMixMode=settings.playback.channelMixMode=ma_channel_mix_mode_simple;settings.playback.calculateLFEFromSpatialChannels=MA_FALSE;
    settings.noFixedSizedCallback=MA_TRUE;settings.noClip=MA_TRUE;settings.noPreSilencedOutputBuffer=MA_TRUE;
    settings.dataCallback=Impl::data;settings.notificationCallback=Impl::notification;settings.pUserData=&p;p.callback=callback;p.owner=owner;
    const bool tuneAAudio = p.context.backend == ma_backend_aaudio && playback && !capture &&
        config.aAudioPlaybackMode != AAudioPlaybackMode::standard;
    if (tuneAAudio) {
        settings.playback.shareMode = ma_share_mode_exclusive;
        // A new route can require a different DSP rate and burst size. Reopen
        // through the owner instead of auto-starting miniaudio's replacement stream.
        settings.aaudio.noAutoStartAfterReroute = MA_TRUE;
    }
    auto result=ma_device_init(&p.context,&settings,&p.device);
    if (result != MA_SUCCESS && tuneAAudio) {
        settings.playback.shareMode = ma_share_mode_shared;
        result = ma_device_init(&p.context,&settings,&p.device);
    }
    if(result!=MA_SUCCESS){p.callback=nullptr;p.owner=nullptr;p.message=std::string("Cannot open audio device: ")+ma_result_description(result);throw std::runtime_error(p.message);}p.deviceReady=true;
    struct OpenRollback{Impl&p;bool committed{};~OpenRollback(){if(!committed)p.close();}} rollback{p};
    if((playback&&p.device.playback.channels!=config.outputChannels)||
       (capture&&p.device.capture.channels!=config.inputChannels))
        throw std::runtime_error("The audio endpoint could not negotiate the requested channel layout");
    if(!config.allowSampleRateConversion&&
       ((playback&&p.device.playback.internalSampleRate!=p.device.sampleRate)||
        (capture&&p.device.capture.internalSampleRate!=p.device.sampleRate)))
        throw std::runtime_error("The selected rate requires backend sample-rate conversion. Choose the device default rate.");
    p.negotiated={portable(p.context.backend),p.device.sampleRate,config.inputChannels,config.outputChannels,
                  config.periodFrames,0,0,0,0,false};
    if(capture){
        p.negotiated.capturePeriodFrames=p.device.capture.internalPeriodSizeInFrames?
            p.device.capture.internalPeriodSizeInFrames:config.periodFrames;
        p.negotiated.capturePeriods=p.device.capture.internalPeriods?p.device.capture.internalPeriods:config.periods;
    }
    if(playback){
        p.negotiated.playbackPeriodFrames=p.device.playback.internalPeriodSizeInFrames?
            p.device.playback.internalPeriodSizeInFrames:config.periodFrames;
        p.negotiated.playbackPeriods=p.device.playback.internalPeriods?p.device.playback.internalPeriods:config.periods;
    }
#if defined(MA_SUPPORT_AAUDIO) && !defined(MA_NO_RUNTIME_LINKING)
    if (tuneAAudio)
        p.readAAudioPlayback(config.aAudioPlaybackMode);
#endif
#if defined(MA_SUPPORT_PULSEAUDIO)
    if(p.context.backend==ma_backend_pulseaudio&&capture&&playback){
        // The native descriptor reports maximum capacity as period count.
        // Latency uses the selected capture target and actual playback target;
        // spare server capacity is headroom, not a forced queue fill.
        const auto getAttr=reinterpret_cast<ma_pa_stream_get_buffer_attr_proc>(p.context.pulse.pa_stream_get_buffer_attr);
        const auto* captureAttr=getAttr(static_cast<ma_pa_stream*>(p.device.pulse.pStreamCapture));
        const auto* playbackAttr=getAttr(static_cast<ma_pa_stream*>(p.device.pulse.pStreamPlayback));
        if(!captureAttr||!playbackAttr)throw std::runtime_error("Cannot inspect negotiated PulseAudio buffers");
        p.negotiated.capturePeriodFrames=captureAttr->fragsize/(config.inputChannels*sizeof(float));p.negotiated.capturePeriods=config.periods;
        p.negotiated.playbackPeriodFrames=playbackAttr->minreq/(config.outputChannels*sizeof(float));
        if(!p.negotiated.capturePeriodFrames||!p.negotiated.playbackPeriodFrames)throw std::runtime_error("Invalid negotiated PulseAudio periods");
        p.negotiated.playbackPeriods=unsigned((std::uint64_t(playbackAttr->tlength)+playbackAttr->minreq-1)/playbackAttr->minreq);
        // Pulse delivers asynchronous capture bursts. The backend's default
        // five-period ring can drop samples even when our callback meets its
        // deadline. Reserve burst headroom off audio, retaining only the same
        // two-period initial lead; capacity is not an added fixed queue delay.
        ma_duplex_rb replacement{};
        if(p.negotiated.capturePeriodFrames>UINT32_MAX/8)throw std::length_error("PulseAudio capture buffer addressing overflow");
        const auto capacity=std::max(8192u,p.negotiated.capturePeriodFrames*8);
        const auto status=ma_pcm_rb_init(p.device.capture.format,p.device.capture.channels,capacity,nullptr,&p.context.allocationCallbacks,&replacement.rb);
        if(status!=MA_SUCCESS)throw std::runtime_error("Cannot prepare capture burst storage");
        const auto lead=p.negotiated.capturePeriodFrames*2;
        ma_duplex_rb_uninit(&p.device.duplexRB);p.device.duplexRB=replacement;
        p.negotiated.captureBridgeFrames=lead;p.negotiated.captureBridgeCapacityFrames=capacity;
        if(p.device.capture.internalFormat!=ma_format_f32||p.device.playback.internalFormat!=ma_format_f32||p.device.capture.internalChannels!=config.inputChannels||p.device.playback.internalChannels!=config.outputChannels||p.device.capture.internalSampleRate!=p.device.sampleRate||p.device.playback.internalSampleRate!=p.device.sampleRate)throw std::runtime_error("PulseAudio did not negotiate the explicit float32 wire format");
        p.pulseQuantum=config.periodFrames;p.pulseInput.resize(std::size_t(p.pulseQuantum)*config.inputChannels);
        auto& api=p.context.pulse;
        reinterpret_cast<ma_pa_stream_set_read_callback_proc>(api.pa_stream_set_read_callback)(static_cast<ma_pa_stream*>(p.device.pulse.pStreamCapture),Impl::pulseRead,&p);
        reinterpret_cast<ma_pa_stream_set_write_callback_proc>(api.pa_stream_set_write_callback)(static_cast<ma_pa_stream*>(p.device.pulse.pStreamPlayback),Impl::pulseWrite,&p);
        const auto underflow=reinterpret_cast<Impl::NotifySetter>(ma_dlsym(ma_context_get_log(&p.context),api.pulseSO,"pa_stream_set_underflow_callback"));
        const auto overflow=reinterpret_cast<Impl::NotifySetter>(ma_dlsym(ma_context_get_log(&p.context),api.pulseSO,"pa_stream_set_overflow_callback"));
        if(!underflow||!overflow)throw std::runtime_error("PulseAudio stream diagnostics are unavailable");
        underflow(static_cast<ma_pa_stream*>(p.device.pulse.pStreamPlayback),Impl::pulseUnderflow,&p);
        overflow(static_cast<ma_pa_stream*>(p.device.pulse.pStreamCapture),Impl::pulseOverflow,&p);
        overflow(static_cast<ma_pa_stream*>(p.device.pulse.pStreamPlayback),Impl::pulsePlaybackOverflow,&p);
    }
#endif
#if defined(MA_SUPPORT_PULSEAUDIO)
    if(p.context.backend==ma_backend_pulseaudio&&playback&&!capture){
        const auto getAttr=reinterpret_cast<ma_pa_stream_get_buffer_attr_proc>(p.context.pulse.pa_stream_get_buffer_attr);
        const auto*attr=getAttr(static_cast<ma_pa_stream*>(p.device.pulse.pStreamPlayback));
        if(!attr||!attr->minreq)throw std::runtime_error("Cannot inspect negotiated playback buffers");
        const auto stride=std::uint64_t(config.outputChannels)*sizeof(float);
        p.negotiated.playbackPeriodFrames=unsigned(attr->minreq/stride);
        p.negotiated.playbackPeriods=unsigned((std::uint64_t(attr->tlength)+attr->minreq-1)/attr->minreq);
        if(!p.negotiated.playbackPeriodFrames)throw std::runtime_error("Invalid negotiated playback period");
        p.negotiated.capturePeriodFrames=p.negotiated.capturePeriods=0;
        if(p.device.playback.internalFormat!=ma_format_f32||p.device.playback.internalChannels!=config.outputChannels||p.device.playback.internalSampleRate!=p.device.sampleRate)throw std::runtime_error("PulseAudio did not negotiate exact float32 playback");
        p.pulseQuantum=config.periodFrames;p.pulseInput.clear();auto&api=p.context.pulse;
        // Use the live writable region for playback-only as well. The generic
        // backend can consume its initial fill and then miss later write demand.
        reinterpret_cast<ma_pa_stream_set_write_callback_proc>(api.pa_stream_set_write_callback)(static_cast<ma_pa_stream*>(p.device.pulse.pStreamPlayback),Impl::pulseWrite,&p);
        const auto underflow=reinterpret_cast<Impl::NotifySetter>(ma_dlsym(ma_context_get_log(&p.context),api.pulseSO,"pa_stream_set_underflow_callback"));
        const auto overflow=reinterpret_cast<Impl::NotifySetter>(ma_dlsym(ma_context_get_log(&p.context),api.pulseSO,"pa_stream_set_overflow_callback"));
        if(!underflow||!overflow)throw std::runtime_error("PulseAudio stream diagnostics are unavailable");
        underflow(static_cast<ma_pa_stream*>(p.device.pulse.pStreamPlayback),Impl::pulseUnderflow,&p);
        overflow(static_cast<ma_pa_stream*>(p.device.pulse.pStreamPlayback),Impl::pulsePlaybackOverflow,&p);

    }
#endif
#if defined(MA_SUPPORT_PULSEAUDIO)
    if(p.context.backend==ma_backend_pulseaudio&&capture&&!playback){
        auto& api=p.context.pulse;
        auto* stream=static_cast<ma_pa_stream*>(p.device.pulse.pStreamCapture);
        const auto getAttr=reinterpret_cast<ma_pa_stream_get_buffer_attr_proc>(api.pa_stream_get_buffer_attr);
        const auto* attr=getAttr(stream);
        if(!attr||attr->fragsize<config.inputChannels*sizeof(float))
            throw std::runtime_error("Cannot inspect negotiated capture buffers");
        p.negotiated.capturePeriodFrames=attr->fragsize/(config.inputChannels*sizeof(float));
        if(p.device.capture.internalFormat!=ma_format_f32||
           p.device.capture.internalChannels!=config.inputChannels||
           p.device.capture.internalSampleRate!=p.device.sampleRate)
            throw std::runtime_error("PulseAudio did not negotiate exact float32 capture");
        const auto overflow=reinterpret_cast<Impl::NotifySetter>(ma_dlsym(ma_context_get_log(&p.context),api.pulseSO,"pa_stream_set_overflow_callback"));
        if(!overflow)throw std::runtime_error("PulseAudio stream diagnostics are unavailable");
        overflow(stream,Impl::pulseOverflow,&p);
    }
#endif
    p.callbacks=p.frames=p.missing=p.overruns=p.maximum=p.captureUnderruns=p.captureOverruns=p.capturePreroll=0;p.serverPlaybackUnderruns=p.serverCaptureOverruns=p.startupPlaybackUnderruns=p.startupCaptureOverruns=p.serverPlaybackOverruns=p.startupPlaybackOverruns=0;p.running=false;p.interrupted=false;rollback.committed=true;return p.negotiated;
}
bool Device::start(){
    auto& p=*impl_;if(!p.deviceReady){p.message="Open the audio device before starting";return false;}
    const auto result=ma_device_start(&p.device);if(result!=MA_SUCCESS){p.message=std::string("Cannot start audio: ")+ma_result_description(result);return false;}
#if defined(MA_SUPPORT_PULSEAUDIO)
    if(p.context.backend==ma_backend_pulseaudio&&p.negotiated.outputChannels){
        const auto deadline=std::chrono::steady_clock::now()+std::chrono::seconds(5);
        while(!p.pulsePlaybackStarted.load(std::memory_order_acquire)&&!p.interrupted.load(std::memory_order_acquire)&&std::chrono::steady_clock::now()<deadline)std::this_thread::sleep_for(std::chrono::milliseconds(1));
        if(!p.pulsePlaybackStarted.load(std::memory_order_acquire)||p.interrupted.load(std::memory_order_acquire)){ma_device_stop(&p.device);p.message="PulseAudio did not start its negotiated audio stream";return false;}
    }
#endif
    p.running.store(true,std::memory_order_release);return true;
}
void Device::stop()noexcept{auto& p=*impl_;p.running.store(false,std::memory_order_release);if(p.deviceReady)ma_device_stop(&p.device);}
void Device::close()noexcept{impl_->close();}bool Device::isOpen()const noexcept{return impl_->deviceReady;}
DeviceFormat Device::format()const{if(!impl_->deviceReady)throw std::logic_error("Audio device is closed");return impl_->negotiated;}
std::optional<AAudioPlaybackInfo> Device::aAudioPlaybackInfo() const noexcept {
#if defined(MA_SUPPORT_AAUDIO) && !defined(MA_NO_RUNTIME_LINKING)
    return impl_->readAAudioPlayback(AAudioPlaybackMode::standard);
#else
    return std::nullopt;
#endif
}
DeviceStats Device::stats()const noexcept{auto& p=*impl_;return {p.callbacks.load(std::memory_order_acquire),p.frames.load(),p.missing.load(),p.overruns.load(),p.maximum.load(),p.running.load(),p.interrupted.load(),p.captureUnderruns.load(),p.captureOverruns.load(),p.capturePreroll.load(),p.serverPlaybackUnderruns.load(),p.serverCaptureOverruns.load(),p.startupPlaybackUnderruns.load(),p.startupCaptureOverruns.load(),p.serverPlaybackOverruns.load(),p.startupPlaybackOverruns.load()};}
std::string Device::error()const{return impl_->message;}
}
