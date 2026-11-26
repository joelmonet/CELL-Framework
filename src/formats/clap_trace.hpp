// Original Cell Framework implementation; see LICENSE.
#pragma once

#include <clap/clap.h>
#include <bit>
#include <cstddef>
#include <cstring>
#include <span>
#include <stdexcept>
#include <string_view>
#include <type_traits>

namespace cell::clap_trace {
inline constexpr std::string_view signature = "CELL-CLAP-TRACE\x01";
inline constexpr std::size_t maxBytes = 32 * 1024 * 1024;
inline constexpr std::uint32_t maxFrames = 65536;
inline constexpr std::uint32_t maxEvents = 8192;
inline constexpr char editorExtension[] = "cell.doctor.editor-edits/1";
using ReplayEditorEdit = bool(CLAP_ABI *)(const clap_plugin_t*, clap_id, std::uint8_t, double);
struct EditorProbe { ReplayEditorEdit edit; };

inline constexpr char allocationExtension[] = "cell.doctor.callback-allocations/1";

struct AllocationProbe {
    void(CLAP_ABI *enable)(const clap_plugin_t*, bool);
    std::uint64_t(CLAP_ABI *count)(const clap_plugin_t*);
};

enum class Op : std::uint8_t {
    activate = 1, deactivate, start, stop, reset, process, flush, load, save, render,
    result, preset, editor, end, incomplete
};

// All scalars are little endian. Serialize members individually so traces do
// not contain pointers, structure padding, or a host/compiler ABI dependency.
static_assert(std::endian::native == std::endian::little);

class Writer {
public:
    explicit Writer(std::span<std::byte> bytes) : bytes_(bytes) {}
    void raw(const void* data, std::size_t count) noexcept {
        if (count > bytes_.size()) {
            invalidate();
            return;
        }
        if (count) std::memcpy(bytes_.data(), data, count);
        bytes_ = bytes_.subspan(count);
    }
    template<class... T> void operator()(const T&... values) noexcept {
        static_assert((std::is_trivially_copyable_v<T> && ...));
        (raw(&values, sizeof(values)), ...);
    }
    void blob(const void* data, std::size_t size) noexcept {
        if (size > maxBytes || (size && !data)) {
            invalidate();
            return;
        }
        (*this)(static_cast<std::uint32_t>(size));
        raw(data, size);
    }
    void string(std::string_view value) noexcept { blob(value.data(), value.size()); }
    void invalidate() noexcept { valid_ = false; }
    bool valid() const noexcept { return valid_; }
    std::size_t remaining() const noexcept { return bytes_.size(); }
private:
    std::span<std::byte> bytes_;
    bool valid_ = true;
};

class Reader {
public:
    explicit Reader(std::span<const std::byte> bytes) : bytes_(bytes) {}
    std::span<const std::byte> raw(std::size_t count) {
        if (count > bytes_.size()) throw std::runtime_error("Truncated CLAP trace");
        const auto result = bytes_.first(count);
        bytes_ = bytes_.subspan(count);
        return result;
    }
    template<class... T> void operator()(T&... values) {
        static_assert((std::is_trivially_copyable_v<T> && ...));
        ((std::memcpy(&values, raw(sizeof(values)).data(), sizeof(values))), ...);
    }
    template<class T> T get() {
        T value{};
        (*this)(value);
        return value;
    }
    std::span<const std::byte> blob() { return raw(get<std::uint32_t>()); }
    std::string_view string() {
        const auto value = blob();
        return {reinterpret_cast<const char*>(value.data()), value.size()};
    }
    bool empty() const noexcept { return bytes_.empty(); }
private:
    std::span<const std::byte> bytes_;
};

template<class Stream, class T> void transportFields(Stream& io, T& event) {
    io(event.flags, event.song_pos_beats, event.song_pos_seconds, event.tempo, event.tempo_inc,
       event.loop_start_beats, event.loop_end_beats, event.loop_start_seconds,
       event.loop_end_seconds, event.bar_start, event.bar_number, event.tsig_num, event.tsig_denom);
}

template<class Stream, class T> void parameterFields(Stream& io, T& event) {
    if constexpr (std::is_same_v<std::remove_const_t<T>, clap_event_param_value_t>)
        io(event.param_id, event.note_id, event.port_index, event.channel, event.key, event.value);
    else
        io(event.param_id, event.note_id, event.port_index, event.channel, event.key, event.amount);
}

template<class Stream, class T> void noteFields(Stream& io, T& event) {
    io(event.note_id, event.port_index, event.channel, event.key);
}

} // namespace cell::clap_trace
