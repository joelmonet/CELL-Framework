// Original Cell Framework implementation; see LICENSE.
#pragma once

#include <cell/plugin.hpp>
#include <cell/json.hpp>
#include <map>
#include <set>

namespace cell::web {
namespace detail {
inline json::Value parameterSnapshotField(const Parameter& field, double value) {
    using V = json::Value;
    V::Array choices;
    for (const auto& choice : field.choices) choices.emplace_back(choice);
    return V(V::Object{{"key", V(field.key)}, {"name", V(field.name)},
        {"unit", V(field.unit)}, {"minimum", V(field.minimum)}, {"maximum", V(field.maximum)},
        {"default", V(field.defaultValue)}, {"step", V(field.step)}, {"value", V(value)},
        {"choices", V(std::move(choices))}});
}
}

// GUI-thread adapter for an explicit allow-list. Owns, and balances, gestures.
class ParameterBridge {
public:
    // For format adapters with their own host-facing parameter API. The fields
    // must outlive the bridge. A write outside a gesture is a complete edit.
    struct Endpoint {
        std::span<const Parameter> fields;
        std::function<double(ParamId)> read;
        std::function<bool(ParamId)> begin;
        std::function<bool(ParamId, double, bool)> write;
        std::function<bool(ParamId)> end;
    };

    ParameterBridge(Engine& engine, std::span<const ParamId> allowed)
        : ParameterBridge(Endpoint{
            engine.parameters().descriptors(),
            [&engine](ParamId id) { return engine.parameters().getAcquire(id); },
            [&engine](ParamId id) { return engine.beginEdits(std::span(&id, 1)); },
            [&engine](ParamId id, double value, bool active) {
                return active ? engine.editValues(std::array{std::pair{id, value}}) : engine.editGesture(id, value);
            },
            [&engine](ParamId id) { return engine.endEdits(std::span(&id, 1)); }
        }, allowed) {}

    ParameterBridge(Endpoint endpoint, std::span<const ParamId> allowed) : endpoint_(std::move(endpoint)) {
        if (allowed.size() > 128) throw std::length_error("Web parameter allow-list exceeds 128");
        if (!endpoint_.read || !endpoint_.begin || !endpoint_.write || !endpoint_.end)
            throw std::invalid_argument("Incomplete web parameter endpoint");
        for (auto id : allowed) {
            const auto field = std::find_if(endpoint_.fields.begin(), endpoint_.fields.end(),
                [id](const Parameter& parameter) { return parameter.id == id; });
            if (field == endpoint_.fields.end() || !allowed_.emplace(id, &*field).second)
                throw std::invalid_argument("Unknown or duplicate web parameter");
        }
    }

    ~ParameterBridge() { cancel(); }
    ParameterBridge(const ParameterBridge&) = delete;
    ParameterBridge& operator=(const ParameterBridge&) = delete;

    bool apply(std::string_view message) {
        try {
            if (message.size() > 65536) return false;
            const auto value = json::read(message, json::Limits{65536, 4096, 65536, 16});
            const auto key = value["key"].text();
            const auto id = stableId(key);
            const auto binding = allowed_.find(id);
            if (binding == allowed_.end() || binding->second->key != key) return false;
            const auto& field = *binding->second;
            const auto type = value["type"].text();
            if (type == "begin") {
                const auto [entry, inserted] = active_.insert(id);
                if (!inserted) return true;
                try {
                    if (endpoint_.begin(id)) return true;
                } catch (...) {
                    active_.erase(entry);
                    throw;
                }
                active_.erase(entry);
                return false;
            }
            if (type == "end") {
                if (!active_.contains(id) || !endpoint_.end(id)) return false;
                active_.erase(id);
                return true;
            }
            if (type != "parameter") return false;
            const auto* number = value["value"].number();
            if (!number || !std::isfinite(*number) || *number < field.minimum || *number > field.maximum ||
                std::abs(field.clamp(*number) - *number) > std::max(1e-9, field.step * 1e-6)) return false;
            return endpoint_.write(id, *number, active_.contains(id));
        } catch (const std::exception&) { return false; }
    }

    void cancel() noexcept {
        for (auto it = active_.begin(); it != active_.end();) {
            try {
                if (endpoint_.end(*it)) {
                    it = active_.erase(it);
                    continue;
                }
            } catch (...) {}
            ++it;
        }
    }

    std::size_t activeGestures() const noexcept { return active_.size(); }

    std::string snapshot() const {
        using V = json::Value;
        V::Array parameters;
        for (const auto& [id, field] : allowed_)
            parameters.push_back(detail::parameterSnapshotField(*field, endpoint_.read(id)));
        const auto text = json::write(V(V::Object{{"type", V("parameters")}, {"parameters", V(std::move(parameters))}}));
        if (text.size() > 65536) throw std::length_error("Web parameter snapshot exceeds 64 KiB");
        return text;
    }

private:
    Endpoint endpoint_;
    std::map<ParamId, const Parameter*> allowed_;
    std::set<ParamId> active_;
};
} // namespace cell::web
