// Original Cell Framework implementation; see LICENSE.
#pragma once
#include <cell/file.hpp>
#include <cell/files.hpp>
#include <cell/json.hpp>
#include <clap/clap.h>
#include <cstring>
#include <filesystem>

#ifdef _WIN32
#include <windows.h>
#else
#include <dlfcn.h>
#endif

namespace cell::clap_session {
using Value = json::Value;
namespace fs = std::filesystem;

inline void require(bool ok, const char* message) {
    if (!ok) throw std::runtime_error(message);
}

inline void writeJson(const File& file, const Value& value) {
    const auto text = json::write(value);
    files::atomicWrite(file.nativePath(), std::as_bytes(std::span(text)));
}

inline Value readJson(const File& file) {
    const auto bytes = files::readBounded(file.nativePath(), 8 * 1024 * 1024);
    return json::read({reinterpret_cast<const char*>(bytes.data()), bytes.size()});
}

struct Module {
    explicit Module(const fs::path& path, const fs::path& bundle = {}) {
#ifdef _WIN32
        handle = LoadLibraryW(path.c_str());
        if (handle) entry = reinterpret_cast<const clap_plugin_entry_t*>(GetProcAddress(handle, "clap_entry"));
#else
        handle = dlopen(path.c_str(), RTLD_NOW | RTLD_LOCAL);
        if (handle) entry = static_cast<const clap_plugin_entry_t*>(dlsym(handle, "clap_entry"));
#endif
        const auto utf8 = (bundle.empty() ? path : bundle).u8string();
        if (!entry || !clap_version_is_compatible(entry->clap_version) || !entry->init(reinterpret_cast<const char*>(utf8.c_str()))) {
            close();
            throw std::runtime_error("Could not load the installed CLAP plug-in");
        }
    }
    ~Module() { entry->deinit(); close(); }
    Module(const Module&) = delete;
    Module& operator=(const Module&) = delete;

    void close() {
        if (!handle) return;
#ifdef _WIN32
        FreeLibrary(handle);
#else
        dlclose(handle);
#endif
    }
#ifdef _WIN32
    HMODULE handle{};
#else
    void* handle{};
#endif
    const clap_plugin_entry_t* entry{};
};

inline const clap_host_t host{
    CLAP_VERSION, nullptr, "Cell compatibility host", "Cell", "", "1",
    [](const clap_host_t*, const char*) -> const void* { return nullptr; },
    [](const clap_host_t*) {}, [](const clap_host_t*) {}, [](const clap_host_t*) {}
};

inline clap_event_param_value_t edit(clap_id id, unsigned offset, double value) {
    return {{sizeof(clap_event_param_value_t), offset, CLAP_CORE_EVENT_SPACE_ID, CLAP_EVENT_PARAM_VALUE, 0},
            id, nullptr, -1, -1, -1, -1, value};
}

inline clap_input_events_t inputEvents(std::vector<clap_event_param_value_t>& events) {
    return {&events, [](const clap_input_events_t* input) {
        return static_cast<std::uint32_t>(static_cast<const std::vector<clap_event_param_value_t>*>(input->ctx)->size());
    }, [](const clap_input_events_t* input, std::uint32_t index) -> const clap_event_header_t* {
        return &static_cast<const std::vector<clap_event_param_value_t>*>(input->ctx)->at(index).header;
    }};
}

inline Value parameterMap(const clap_plugin_t* plugin, const clap_plugin_params_t* params) {
    Value::Object result;
    for (std::uint32_t index = 0; index < params->count(plugin); ++index) {
        clap_param_info_t info{};
        require(params->get_info(plugin, index, &info), "CLAP parameter enumeration failed");
        const auto inserted = result.emplace(std::to_string(info.id), Value(Value::Object{
            {"name", Value(info.name)}, {"module", Value(info.module)}, {"flags", Value(info.flags)},
            {"minimum", Value(info.min_value)}, {"maximum", Value(info.max_value)},
            {"default", Value(info.default_value)}
        })).second;
        require(inserted, "CLAP reported duplicate parameter IDs");
    }
    require(!result.empty(), "CLAP reported no parameters");
    return Value(std::move(result));
}

struct Plugin {
    Plugin(const Module& module, const std::string& id) {
        const auto* factory = static_cast<const clap_plugin_factory_t*>(module.entry->get_factory(CLAP_PLUGIN_FACTORY_ID));
        require(factory && factory->get_plugin_count(factory) == 1, "CLAP factory is missing the reference plug-in");
        const auto* descriptor = factory->get_plugin_descriptor(factory, 0);
        require(descriptor && descriptor->id, "CLAP descriptor has no ID");
        require(id.empty() || id == descriptor->id, "Released CLAP plug-in ID changed");
        instance = factory->create_plugin(factory, &host, descriptor->id);
        require(instance != nullptr, "Could not instantiate the saved CLAP plug-in ID");
        if (!instance->init(instance)) {
            instance->destroy(instance);
            throw std::runtime_error("Could not initialize the saved CLAP plug-in");
        }
    }
    ~Plugin() { instance->destroy(instance); }
    Plugin(const Plugin&) = delete;
    Plugin& operator=(const Plugin&) = delete;

    template<class Extension>
    const Extension* extension(const char* id) const {
        const auto* result = static_cast<const Extension*>(instance->get_extension(instance, id));
        require(result != nullptr, id);
        return result;
    }

    void set(clap_id id, double value) const {
        std::vector events{edit(id, 0, value)};
        const auto input = inputEvents(events);
        extension<clap_plugin_params_t>(CLAP_EXT_PARAMS)->flush(instance, &input, nullptr);
    }

    double value(clap_id id) const {
        double result{};
        require(extension<clap_plugin_params_t>(CLAP_EXT_PARAMS)->get_value(instance, id, &result),
                "Saved parameter ID is missing");
        return result;
    }

    void save(const File& file) const {
        std::vector<std::byte> data;
        const clap_ostream_t stream{&data, [](const clap_ostream_t* out, const void* src, std::uint64_t size) -> std::int64_t {
            auto& buffer = *static_cast<std::vector<std::byte>*>(out->ctx);
            if (size > stateLimit - buffer.size() || (size && !src)) return -1;
            try {
                const auto* bytes = static_cast<const std::byte*>(src);
                if (size) buffer.insert(buffer.end(), bytes, bytes + size);
                return static_cast<std::int64_t>(size);
            } catch (...) { return -1; }
        }};
        require(extension<clap_plugin_state_t>(CLAP_EXT_STATE)->save(instance, &stream) && !data.empty(),
                "CLAP state capture failed");
        files::atomicWrite(file.nativePath(), data);
    }

    void load(const File& file) const {
        auto data = files::readBounded(file.nativePath(), stateLimit);
        std::span<const std::byte> remaining(data);
        const clap_istream_t stream{&remaining, [](const clap_istream_t* in, void* dst, std::uint64_t size) -> std::int64_t {
            auto& bytes = *static_cast<std::span<const std::byte>*>(in->ctx);
            const auto count = std::min<std::uint64_t>(size, bytes.size());
            if (count) std::memcpy(dst, bytes.data(), count);
            bytes = bytes.subspan(static_cast<std::size_t>(count));
            return static_cast<std::int64_t>(count);
        }};
        require(extension<clap_plugin_state_t>(CLAP_EXT_STATE)->load(instance, &stream) && remaining.empty(),
                "Candidate could not read the saved state or preset");
    }

    static constexpr std::size_t stateLimit = 128 * 1024 * 1024;
    const clap_plugin_t* instance{};
};

} // namespace cell::clap_session
