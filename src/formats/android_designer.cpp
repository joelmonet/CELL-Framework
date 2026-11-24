// Original Cell Framework implementation; see LICENSE.
#include <cell/audio_device.hpp>
#include <cell/detail/web_document.hpp>
#include <cell/web_meter.hpp>
#include "cell_android_web_bindings.hpp"
#include "cell_android_web_page.hpp"
#include <jni.h>
#include <numbers>

namespace {
using cell::web::detail::maxMessageBytes;
constexpr auto gainId = cell::stableId("gain");
constexpr unsigned channels = 2;
constexpr unsigned blockFrames = 512;
constexpr auto bridgeScript = R"HTML(<script>
(() => {
    'use strict';
    const listeners = [];
    let port;
    window.cell = Object.freeze({
        postMessage(value) {
            const json = JSON.stringify(value);
            if (port && json.length <= 65536) port.postMessage(json);
        },
        onMessage(fn) {
            if (typeof fn !== 'function') throw new TypeError('Expected function');
            listeners.push(fn);
            return () => { const index = listeners.indexOf(fn); if (index >= 0) listeners.splice(index, 1); };
        }
    });
    window.addEventListener('message', (event) => {
        if (port || event.data !== 'cell-editor' || event.ports.length !== 1) return;
        port = event.ports[0];
        port.onmessage = ({data}) => { const value = JSON.parse(data); listeners.slice().forEach(fn => fn(value)); };
        port.postMessage('cell-ready');
    });
})();
</script>)HTML";

std::string document() {
    const std::string_view html(reinterpret_cast<const char*>(cell::assets::androidEditorPage),
                                sizeof(cell::assets::androidEditorPage));
    const auto head = html.find("<head>");
    if (html.find('\0') != html.npos || head == html.npos)
        throw std::invalid_argument("Use a UTF-8 WebView HTML export from Cell UI Designer");
    const auto insertion = head + std::string_view("<head>").size();
    std::string page(html.substr(0, insertion));
    page += cell::web::detail::offlineContentPolicy;
    page += bridgeScript;
    page += html.substr(insertion);
    return page;
}

class Session {
public:
    explicit Session(double gain) {
        if (std::isfinite(gain)) engine_.parameters().set(gainId, gain);
        meter_.setEnabled(true);
    }

    ~Session() { stop(); }

    void start() {
        if (device_.isOpen()) return;
        try {
            cell::audio::DeviceConfig config;
            config.inputChannels = 0;
            config.outputChannels = channels;
            config.sampleRate = 0;
            config.aAudioPlaybackMode = cell::audio::AAudioPlaybackMode::lowLatency;
            const auto format = device_.open(config, [](void* owner, float* output, const float*, unsigned frames) noexcept {
                static_cast<Session*>(owner)->render(output, frames);
            }, this);
            if (format.outputChannels != channels) throw std::runtime_error("Stereo output unavailable");
            engine_.prepare(format.sampleRate, blockFrames, channels);
            phase_ = 0.;
            phaseStep_ = 2. * std::numbers::pi * 440. / format.sampleRate;
            if (!device_.start()) throw std::runtime_error(device_.error());
        } catch (...) {
            stop();
            throw;
        }
    }

    void stop() noexcept {
        device_.close();
        meter_.clear();
    }

    bool playing() const noexcept { return device_.stats().running && !device_.stats().interrupted; }
    double gain() const noexcept { return engine_.parameters().getAcquire(gainId); }

    void apply(std::string_view message) {
        bridge_.apply(message);
        // The engine publishes values in its parameter bank. This standalone
        // host has no DAW automation queue, so consume edit notifications here.
        flushEdits();
        lastParameters_.clear();
    }

    void cancel() {
        bridge_.cancel();
        flushEdits();
        lastParameters_.clear();
        lastMeters_.clear();
    }

    std::string snapshot(bool meters) {
        auto next = meters ? meter_.snapshot() : bridge_.snapshot();
        auto& last = meters ? lastMeters_ : lastParameters_;
        if (next == last) return {};
        last = next;
        return next;
    }

private:
    void flushEdits() noexcept {
        cell::ParameterEdit edit;
        while (engine_.pollUiEdit(edit)) {}
    }

    void render(float* output, unsigned frames) noexcept {
        for (unsigned cursor = 0; cursor < frames;) {
            const auto count = std::min(frames - cursor, blockFrames);
            for (unsigned frame = 0; frame < count; ++frame) {
                const auto sample = static_cast<float>(.04 * std::sin(phase_));
                phase_ += phaseStep_;
                if (phase_ >= 2. * std::numbers::pi) phase_ -= 2. * std::numbers::pi;
                left_[frame] = right_[frame] = sample;
            }
            const cell::AudioBlock<float> block{buffers_.data(), channels, count, 0};
            engine_.process(block);
            meter_.publish(block);
            for (unsigned frame = 0; frame < count; ++frame) {
                output[channels * (cursor + frame)] = left_[frame];
                output[channels * (cursor + frame) + 1] = right_[frame];
            }
            cursor += count;
        }
    }

    cell::Engine engine_{cell::plugin::descriptor(), cell::plugin::createProcessor()};
#if defined(CELL_DESIGNER_HAS_ACTIONS) || defined(CELL_DESIGNER_HAS_RULE_SOURCES)
    cell::web::ParameterBridge bridge_ = cell::designer::bind(engine_);
#else
    cell::web::ParameterBridge bridge_{engine_, cell::designer::parameters};
#endif
    cell::web::OutputMeter meter_;
    cell::audio::Device device_;
    std::array<float, blockFrames> left_{}, right_{};
    std::array<float*, channels> buffers_{left_.data(), right_.data()};
    double phase_{}, phaseStep_{};
    std::string lastParameters_, lastMeters_;
};

Session* session(jlong handle) { return reinterpret_cast<Session*>(handle); }

jbyteArray javaBytes(JNIEnv* env, std::string_view data) {
    const auto size = static_cast<jsize>(data.size());
    auto bytes = env->NewByteArray(size);
    if (bytes && size) env->SetByteArrayRegion(bytes, 0, size, reinterpret_cast<const jbyte*>(data.data()));
    return bytes;
}

void appError(JNIEnv* env, const char* message) {
    if (const auto type = env->FindClass("java/lang/IllegalStateException")) env->ThrowNew(type, message);
}
}

extern "C" JNIEXPORT jlong JNICALL Java_org_cell_examples_designer_MainActivity_nativeCreate(JNIEnv* env, jclass, jdouble gain) {
    try { return reinterpret_cast<jlong>(new Session(gain)); }
    catch (...) { appError(env, "Could not initialize the Cell gain editor"); return 0; }
}

extern "C" JNIEXPORT void JNICALL Java_org_cell_examples_designer_MainActivity_nativeDestroy(JNIEnv*, jclass, jlong handle) {
    delete session(handle);
}

extern "C" JNIEXPORT jbyteArray JNICALL Java_org_cell_examples_designer_MainActivity_nativeDocument(JNIEnv* env, jclass) {
    try { return javaBytes(env, document()); }
    catch (...) { appError(env, "Could not load the Designer export"); return nullptr; }
}

extern "C" JNIEXPORT void JNICALL Java_org_cell_examples_designer_MainActivity_nativeStart(JNIEnv* env, jclass, jlong handle) {
    try { session(handle)->start(); }
    catch (...) { appError(env, "Could not start audio output"); }
}

extern "C" JNIEXPORT void JNICALL Java_org_cell_examples_designer_MainActivity_nativeStop(JNIEnv*, jclass, jlong handle) {
    session(handle)->stop();
}

extern "C" JNIEXPORT jboolean JNICALL Java_org_cell_examples_designer_MainActivity_nativePlaying(JNIEnv*, jclass, jlong handle) {
    return session(handle)->playing();
}

extern "C" JNIEXPORT jdouble JNICALL Java_org_cell_examples_designer_MainActivity_nativeGain(JNIEnv*, jclass, jlong handle) {
    return session(handle)->gain();
}

extern "C" JNIEXPORT void JNICALL Java_org_cell_examples_designer_MainActivity_nativeEdit(JNIEnv* env, jclass, jlong handle, jbyteArray data) {
    try {
        const auto size = env->GetArrayLength(data);
        if (size < 1 || static_cast<std::size_t>(size) > maxMessageBytes) return;
        std::string text(size, '\0');
        env->GetByteArrayRegion(data, 0, size, reinterpret_cast<jbyte*>(text.data()));
        if (!env->ExceptionCheck()) session(handle)->apply(text);
    } catch (...) { appError(env, "Could not apply the Designer edit"); }
}

extern "C" JNIEXPORT void JNICALL Java_org_cell_examples_designer_MainActivity_nativeCancel(JNIEnv*, jclass, jlong handle) {
    session(handle)->cancel();
}

extern "C" JNIEXPORT jbyteArray JNICALL Java_org_cell_examples_designer_MainActivity_nativeSnapshot(JNIEnv* env, jclass, jlong handle, jboolean meters) {
    try {
        const auto data = session(handle)->snapshot(meters);
        return data.empty() ? nullptr : javaBytes(env, data);
    } catch (...) { appError(env, "Could not read Designer state"); return nullptr; }
}
