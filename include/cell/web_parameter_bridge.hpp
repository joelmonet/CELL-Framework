// Original Cell Framework implementation; see LICENSE.
#pragma once

#include <cell/plugin.hpp>
#include <cell/json.hpp>
#include <cell/commands.hpp>
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

inline const ProgramSlot* findProgram(const ProgramCatalog& catalog, ProgramAddress address) {
    for (const auto& bank : catalog.banks)
        if (bank.number == address.bank)
            for (const auto& program : bank.programs)
                if (program.number == address.program) return &program;
    return nullptr;
}

inline void validateProgramCatalog(const ProgramCatalog& catalog) {
    const auto validName = [](std::string_view name) {
        return !name.empty() && name.size() <= 80 && std::any_of(name.begin(), name.end(), [](unsigned char c) { return c > ' '; }) &&
            std::none_of(name.begin(), name.end(), [](unsigned char c) { return c < ' ' || c == 127; });
    };
    if (catalog.banks.size() > 64) throw std::length_error("Too many web program banks");
    std::set<unsigned> bankNumbers;
    std::size_t count{};
    for (const auto& bank : catalog.banks) {
        if (bank.number > 16383 || !bankNumbers.insert(bank.number).second || !validName(bank.name))
            throw std::invalid_argument("Invalid or duplicate web program bank");
        if (bank.programs.size() > 128 || (count += bank.programs.size()) > 512)
            throw std::length_error("Too many web programs");
        std::set<unsigned> numbers;
        for (const auto& program : bank.programs) {
            if (program.number > 127 || !numbers.insert(program.number).second || !validName(program.name))
                throw std::invalid_argument("Invalid or duplicate web program");
        }
    }
    if (catalog.active && !findProgram(catalog, *catalog.active))
        throw std::invalid_argument("Active web program is outside the catalog");
}

inline json::Value programSnapshot(const ProgramCatalog& catalog) {
    using V = json::Value;
    validateProgramCatalog(catalog);
    V::Array banks;
    for (const auto& bank : catalog.banks) {
        V::Array programs;
        for (const auto& program : bank.programs)
            programs.emplace_back(V::Object{{"number", V(program.number)}, {"name", V(program.name)}});
        banks.emplace_back(V::Object{{"number", V(bank.number)}, {"name", V(bank.name)}, {"programs", V(std::move(programs))}});
    }
    V active;
    if (catalog.active)
        active = V(V::Object{{"bank", V(catalog.active->bank)}, {"program", V(catalog.active->program)}});
    return V(V::Object{{"type", V("program-memory")}, {"banks", V(std::move(banks))}, {"active", std::move(active)}});
}

inline bool validActionKey(std::string_view key) {
    const auto identifier = [](char c) {
        return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9');
    };
    return !key.empty() && key.size() <= 80 && identifier(key.front()) &&
        std::all_of(key.begin(), key.end(), [identifier](char c) {
            return identifier(c) || c == '-' || c == '_' || c == '.';
        });
}

inline std::optional<ProgramCatalog> processorPrograms(const Engine& engine, const ParameterBank& parameters) {
    if (const auto* memory = dynamic_cast<const ProgramMemory*>(&engine.processor()))
        return memory->programs(parameters);
    return {};
}

inline bool recallProcessorProgram(Engine& engine, ProgramAddress address) {
    if (engine.activeGestures()) return false;
    auto* memory = dynamic_cast<ProgramMemory*>(&engine.processor());
    return memory && memory->recallProgram(engine, address);
}

inline bool processorSupportsAction(const Engine& engine, std::string_view key, ActionMode mode) {
    const auto* target = dynamic_cast<const ActionTarget*>(&engine.processor());
    return target && target->supportsWebAction(key, mode);
}

inline bool processorAction(Engine& engine, std::string_view key, ActionPhase phase) {
    auto* target = dynamic_cast<ActionTarget*>(&engine.processor());
    return target && target->webAction(engine, key, phase);
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
        std::function<std::optional<ProgramCatalog>()> programs{};
        std::function<bool(ProgramAddress)> recallProgram{};
        std::function<bool(ParamId, double, double)> beginMomentary{};
        std::function<bool(std::string_view, ActionMode)> supportsAction{};
        std::function<bool(std::string_view, ActionPhase)> action{};
    };

    // Read-only IDs are a subset of the snapshot allow-list and cannot acquire gestures.
    ParameterBridge(Engine& engine, std::span<const ParamId> allowed, bool exposePrograms = true,
                    std::span<const ActionBinding> actions = {}, std::span<const ParamId> readOnly = {})
        : ParameterBridge(Endpoint{
            engine.parameters().descriptors(),
            [&engine](ParamId id) { return engine.parameters().getAcquire(id); },
            [&engine](ParamId id) { return engine.beginEdits(std::span(&id, 1)); },
            [&engine](ParamId id, double value, bool active) {
                return active ? engine.editValues(std::array{std::pair{id, value}}) : engine.editGesture(id, value);
            },
            [&engine](ParamId id) { return engine.endEdits(std::span(&id, 1)); },
            [&engine, exposePrograms]() -> std::optional<ProgramCatalog> {
                return exposePrograms ? detail::processorPrograms(engine, engine.parameters()) : std::nullopt;
            },
            [&engine, exposePrograms](ProgramAddress address) {
                return exposePrograms && detail::recallProcessorProgram(engine, address);
            },
            [&engine](ParamId id, double press, double release) {
                return engine.beginMomentary(id, press, release);
            },
            [&engine](std::string_view key, ActionMode mode) {
                return detail::processorSupportsAction(engine, key, mode);
            },
            [&engine](std::string_view key, ActionPhase phase) {
                return detail::processorAction(engine, key, phase);
            }
        }, allowed, actions, readOnly) {}

    ParameterBridge(Endpoint endpoint, std::span<const ParamId> allowed, std::span<const ActionBinding> actions = {},
                    std::span<const ParamId> readOnly = {})
        : endpoint_(std::move(endpoint)) {
        if (allowed.size() > 128) throw std::length_error("Web parameter allow-list exceeds 128");
        if (actions.size() > 128) throw std::length_error("Web action allow-list exceeds 128");
        if (!endpoint_.read || !endpoint_.begin || !endpoint_.write || !endpoint_.end)
            throw std::invalid_argument("Incomplete web parameter endpoint");
        if (static_cast<bool>(endpoint_.programs) != static_cast<bool>(endpoint_.recallProgram))
            throw std::invalid_argument("Incomplete web program endpoint");
        if (static_cast<bool>(endpoint_.supportsAction) != static_cast<bool>(endpoint_.action))
            throw std::invalid_argument("Incomplete web action endpoint");
        for (auto id : allowed) {
            const auto field = std::find_if(endpoint_.fields.begin(), endpoint_.fields.end(),
                [id](const Parameter& parameter) { return parameter.id == id; });
            if (field == endpoint_.fields.end() || !allowed_.emplace(id, &*field).second)
                throw std::invalid_argument("Unknown or duplicate web parameter");
        }
        for (auto id : readOnly)
            if (!allowed_.contains(id) || !readOnly_.insert(id).second)
                throw std::invalid_argument("Unknown or duplicate read-only web parameter");
        for (const auto& action : actions) {
            if (!detail::validActionKey(action.key) ||
                (action.mode != ActionMode::invoke && action.mode != ActionMode::momentary) ||
                !allowedActions_.emplace(std::string(action.key), action.mode).second)
                throw std::invalid_argument("Invalid or duplicate web action");
        }
    }

    ~ParameterBridge() { cancel(); }
    ParameterBridge(const ParameterBridge&) = delete;
    ParameterBridge& operator=(const ParameterBridge&) = delete;

    bool apply(std::string_view message) {
        try {
            if (message.size() > 65536) return false;
            const auto value = json::read(message, json::Limits{65536, 4096, 65536, 16});
            const auto type = value["type"].text();
            if (type == "recall-program") return recall(value);
            if (type == "action") return applyAction(value);
            const auto key = value["key"].text();
            const auto id = stableId(key);
            const auto binding = allowed_.find(id);
            if (binding == allowed_.end() || binding->second->key != key || readOnly_.contains(id)) return false;
            const auto& field = *binding->second;
            if (type == "begin" || type == "momentary-press") {
                const bool momentary = type == "momentary-press";
                if (momentary && (!endpoint_.beginMomentary || field.minimum >= field.maximum)) return false;
                const auto [entry, inserted] = active_.emplace(id, momentary);
                if (!inserted) return entry->second == momentary;
                try {
                    if (momentary ? endpoint_.beginMomentary(id, field.maximum, field.minimum) : endpoint_.begin(id))
                        return true;
                } catch (...) {
                    active_.erase(entry);
                    throw;
                }
                active_.erase(entry);
                return false;
            }
            const auto gesture = active_.find(id);
            if (type == "end" || type == "momentary-release") {
                if (gesture == active_.end() || gesture->second != (type == "momentary-release") ||
                    !endpoint_.end(id)) return false;
                active_.erase(gesture);
                return true;
            }
            if (type != "parameter" || (gesture != active_.end() && gesture->second)) return false;
            const auto* number = value["value"].number();
            if (!number || !std::isfinite(*number) || *number < field.minimum || *number > field.maximum ||
                std::abs(field.clamp(*number) - *number) > std::max(1e-9, field.step * 1e-6)) return false;
            return endpoint_.write(id, *number, gesture != active_.end());
        } catch (const std::exception&) { return false; }
    }

    void cancel() noexcept {
        for (auto it = active_.begin(); it != active_.end();) {
            try {
                if (endpoint_.end(it->first)) {
                    it = active_.erase(it);
                    continue;
                }
            } catch (...) {}
            ++it;
        }
        for (auto it = activeActions_.begin(); it != activeActions_.end();) {
            try {
                if (endpoint_.action(*it, ActionPhase::release)) {
                    it = activeActions_.erase(it);
                    continue;
                }
            } catch (...) {}
            ++it;
        }
    }

    std::size_t activeGestures() const noexcept { return active_.size(); }
    std::size_t activeActions() const noexcept { return activeActions_.size(); }

    std::string snapshot() const {
        using V = json::Value;
        V::Array parameters;
        for (const auto& [id, field] : allowed_)
            parameters.push_back(detail::parameterSnapshotField(*field, endpoint_.read(id)));
        V::Object snapshot{{"type", V("parameters")}, {"parameters", V(std::move(parameters))}};
        if (endpoint_.beginMomentary) snapshot.emplace("momentary", V(true));
        V::Array actions;
        if (endpoint_.supportsAction)
            for (const auto& [key, mode] : allowedActions_)
                if (endpoint_.supportsAction(key, mode))
                    actions.emplace_back(V::Object{{"key", V(key)}, {"mode", V(mode == ActionMode::invoke ? "invoke" : "momentary")}});
        if (!actions.empty()) snapshot.emplace("actions", V(std::move(actions)));
        if (endpoint_.programs)
            if (const auto programs = endpoint_.programs())
                snapshot.emplace("programs", detail::programSnapshot(*programs));
        const auto text = json::write(V(std::move(snapshot)));
        if (text.size() > 65536) throw std::length_error("Web parameter snapshot exceeds 64 KiB");
        return text;
    }

private:
    bool applyAction(const json::Value& message) {
        const auto key = message["key"].text();
        const auto phase = message["phase"].text();
        if (!endpoint_.action || !detail::validActionKey(key)) return false;
        if (phase == "release") {
            const auto active = activeActions_.find(key);
            if (active == activeActions_.end() || !endpoint_.action(*active, ActionPhase::release)) return false;
            activeActions_.erase(active);
            return true;
        }
        if (phase != "invoke" && phase != "press") return false;
        const bool momentary = phase == "press";
        const auto mode = momentary ? ActionMode::momentary : ActionMode::invoke;
        if (!allowedActions_.contains({std::string(key), mode}) || !endpoint_.supportsAction(key, mode)) return false;
        if (!momentary) return endpoint_.action(key, ActionPhase::invoke);
        const auto [active, inserted] = activeActions_.emplace(key);
        if (!inserted) return true;
        try {
            if (endpoint_.action(*active, ActionPhase::press)) return true;
        } catch (...) {
            activeActions_.erase(active);
            throw;
        }
        activeActions_.erase(active);
        return false;
    }

    bool recall(const json::Value& message) {
        if (!endpoint_.programs || !active_.empty()) return false;
        const auto* bank = message["bank"].unsignedInteger();
        const auto* program = message["program"].unsignedInteger();
        if (!bank || !program || *bank > 16383 || *program > 127) return false;
        const ProgramAddress address{static_cast<unsigned>(*bank), static_cast<unsigned>(*program)};
        const auto programs = endpoint_.programs();
        if (!programs) return false;
        detail::validateProgramCatalog(*programs);
        return detail::findProgram(*programs, address) && endpoint_.recallProgram(address);
    }

    Endpoint endpoint_;
    std::map<ParamId, const Parameter*> allowed_;
    std::set<ParamId> readOnly_;
    std::map<ParamId, bool> active_;
    std::set<std::pair<std::string, ActionMode>> allowedActions_;
    std::set<std::string, std::less<>> activeActions_;
};
} // namespace cell::web
