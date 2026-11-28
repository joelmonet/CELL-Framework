// Original Cell Framework implementation; see LICENSE.
#pragma once
#include "clap_trace.hpp"
#include <cell/files.hpp>
#include <atomic>
#include <cstdlib>
#include <filesystem>
#include <cstdio>
#include <memory>
#include <utility>
#include <vector>

namespace cell::clap_trace {

template<class T, class Write>
void eventBody(Writer& out, const clap_event_header_t& header, Write&& write) noexcept {
    if (header.size < sizeof(T)) {
        out.invalidate();
        return;
    }
    write(*reinterpret_cast<const T*>(&header));
}

inline void events(Writer& out, const clap_input_events_t* input) noexcept {
    const auto count = input && input->size && input->get ? input->size(input) : 0;
    out(count);
    if (count > maxEvents) {
        out.invalidate();
        return;
    }
    for (std::uint32_t index = 0; index < count && out.valid(); ++index) {
        const auto* head = input->get(input, index);
        if (!head || head->space_id != CLAP_CORE_EVENT_SPACE_ID) {
            out.invalidate();
            return;
        }
        out(head->type, head->time, head->flags);
        switch (head->type) {
        case CLAP_EVENT_PARAM_VALUE:
            eventBody<clap_event_param_value_t>(out, *head, [&](const auto& e) { parameterFields(out, e); }); break;
        case CLAP_EVENT_PARAM_MOD:
            eventBody<clap_event_param_mod_t>(out, *head, [&](const auto& e) { parameterFields(out, e); }); break;
        case CLAP_EVENT_PARAM_GESTURE_BEGIN: case CLAP_EVENT_PARAM_GESTURE_END:
            eventBody<clap_event_param_gesture_t>(out, *head, [&](const auto& e) { out(e.param_id); }); break;
        case CLAP_EVENT_MIDI:
            eventBody<clap_event_midi_t>(out, *head, [&](const auto& e) { out(e.port_index, e.data); }); break;
        case CLAP_EVENT_MIDI2:
            eventBody<clap_event_midi2_t>(out, *head, [&](const auto& e) { out(e.port_index, e.data); }); break;
        case CLAP_EVENT_MIDI_SYSEX:
            eventBody<clap_event_midi_sysex_t>(out, *head, [&](const auto& e) {
                out(e.port_index); out.blob(e.buffer, e.size);
            }); break;
        case CLAP_EVENT_NOTE_ON: case CLAP_EVENT_NOTE_OFF: case CLAP_EVENT_NOTE_CHOKE: case CLAP_EVENT_NOTE_END:
            eventBody<clap_event_note_t>(out, *head, [&](const auto& e) { noteFields(out, e); out(e.velocity); }); break;
        case CLAP_EVENT_NOTE_EXPRESSION:
            eventBody<clap_event_note_expression_t>(out, *head, [&](const auto& e) {
                out(e.expression_id); noteFields(out, e); out(e.value);
            }); break;
        case CLAP_EVENT_TRANSPORT:
            eventBody<clap_event_transport_t>(out, *head, [&](const auto& e) { transportFields(out, e); }); break;
        default:
            out.invalidate();
        }
    }
}

class Capture {
public:
    static std::unique_ptr<Capture> open(std::string_view id) noexcept {
#if defined(_WIN32)
        const auto* path = _wgetenv(L"CELL_DOCTOR_TRACE_DIR");
#else
        const auto* path = std::getenv("CELL_DOCTOR_TRACE_DIR");
#endif
        const auto* filter = std::getenv("CELL_DOCTOR_PLUGIN_ID");
        if (!path || !*path || (filter && id != filter)) return {};
        try {
            const std::filesystem::path root(path);
            std::filesystem::create_directories(root);
            for (unsigned index = 1; index <= 1024; ++index) {
                const auto directory = root / ("instance-" + std::to_string(index));
                if (std::filesystem::create_directory(directory))
                    return std::unique_ptr<Capture>(new Capture(directory / "host.celltrace", id,
                                                                audioRequested()));
            }
            throw std::runtime_error("No free capture instance slots");
        } catch (const std::exception& error) {
            std::fprintf(stderr, "Cell Doctor recording: %s\n", error.what());
            return {};
        }
    }

    bool isCallingHere() const noexcept { return calling_ == this; }

    // CLAP serializes audio callbacks. A main-thread state or render change
    // can arrive concurrently. Preserve an explicit incomplete trace in that
    // case instead of inventing an order or blocking an audio callback.
    class Call {
    public:
        explicit Call(Capture* capture) noexcept : capture_(capture) {
            if (capture_ && (capture_->incomplete_.load(std::memory_order_relaxed) ||
                             capture_->busy_.test_and_set(std::memory_order_acquire))) {
                capture_->incomplete_.store(true, std::memory_order_relaxed);
                capture_ = nullptr;
            }
            if (capture_) previous_ = std::exchange(calling_, capture_);
        }
        Call(const Call&) = delete;
        ~Call() {
            if (capture_) {
                calling_ = previous_;
                capture_->busy_.clear(std::memory_order_release);
            }
        }
        template<class Encode> void record(Op op, Encode&& encode) noexcept {
            if (!capture_) return;
            Writer writer(std::span(capture_->bytes_).subspan(capture_->used_, maxBytes - 1 - capture_->used_));
            const auto capacity = writer.remaining();
            writer(op);
            encode(writer);
            if (writer.valid()) capture_->used_ += capacity - writer.remaining();
            else capture_->incomplete_.store(true, std::memory_order_relaxed);
        }
        bool audio() const noexcept { return capture_ && capture_->audio_; }
        template<class... T> void write(Op op, const T&... values) noexcept {
            record(op, [&](Writer& writer) { writer(values...); });
        }
    private:
        Capture* capture_;
        Capture* previous_{};
    };

    ~Capture() {
        const auto status = incomplete_.load(std::memory_order_relaxed) ? Op::incomplete : Op::end;
        bytes_[used_++] = static_cast<std::byte>(status);
        try {
            files::atomicWrite(path_, std::span(bytes_).first(used_));
        } catch (const std::exception& error) {
            std::fprintf(stderr, "Cell Doctor recording: %s\n", error.what());
        }
    }
private:
    static bool audioRequested() noexcept {
        const auto* setting = std::getenv("CELL_DOCTOR_TRACE_AUDIO");
        return setting && std::string_view(setting) == "1";
    }
    Capture(std::filesystem::path path, std::string_view id, bool audio)
        : path_(std::move(path)), bytes_(maxBytes), audio_(audio) {
        Writer writer(bytes_);
        writer.raw(signature.data(), signature.size());
        writer.string(id);
        if (!writer.valid()) throw std::runtime_error("Invalid CLAP plug-in identity");
        used_ = bytes_.size() - writer.remaining();
    }
    static inline thread_local Capture* calling_{};
    std::filesystem::path path_;
    std::vector<std::byte> bytes_;
    std::size_t used_{};
    bool audio_;
    std::atomic_flag busy_{};
    std::atomic<bool> incomplete_{};
};

} // namespace cell::clap_trace
