// Original Cell Framework implementation; see LICENSE.
#pragma once

#include <cell/plugin.hpp>
#include <cell/json.hpp>
#include <cell/commands.hpp>
#include <map>
#include <set>
#include <utility>

namespace cell::web {
inline constexpr std::size_t maxEditorFileBytes = 45 * 1024;
struct EditorFileRequest {
    bool save{};
    std::vector<std::string> filters;
    std::vector<std::byte> bytes;
};

namespace detail {
inline constexpr std::string_view base64Digits = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

inline std::string encodeFileBytes(std::span<const std::byte> bytes) {
    std::string result;
    result.reserve(((bytes.size() + 2) / 3) * 4);
    for (std::size_t i = 0; i < bytes.size(); i += 3) {
        const auto remaining = bytes.size() - i;
        const auto a = std::to_integer<unsigned>(bytes[i]);
        const auto b = remaining > 1 ? std::to_integer<unsigned>(bytes[i + 1]) : 0;
        const auto c = remaining > 2 ? std::to_integer<unsigned>(bytes[i + 2]) : 0;
        result += base64Digits[a >> 2];
        result += base64Digits[((a & 3) << 4) | (b >> 4)];
        result += remaining > 1 ? base64Digits[((b & 15) << 2) | (c >> 6)] : '=';
        result += remaining > 2 ? base64Digits[c & 63] : '=';
    }
    return result;
}

inline std::optional<std::vector<std::byte>> decodeFileBytes(std::string_view text) {
    if (text.size() % 4 || text.size() > maxEditorFileBytes / 3 * 4) return {};
    std::vector<std::byte> result;
    result.reserve(text.size() / 4 * 3);
    for (std::size_t i = 0; i < text.size(); i += 4) {
        const auto a = base64Digits.find(text[i]), b = base64Digits.find(text[i + 1]);
        const auto c = base64Digits.find(text[i + 2]), d = base64Digits.find(text[i + 3]);
        if (a == base64Digits.npos || b == base64Digits.npos) return {};
        const bool final = i + 4 == text.size();
        if (c == base64Digits.npos &&
            (!final || text[i + 2] != '=' || text[i + 3] != '=' || (b & 15))) return {};
        if (c != base64Digits.npos && d == base64Digits.npos &&
            (!final || text[i + 3] != '=' || (c & 3))) return {};
        result.push_back(std::byte((a << 2) | (b >> 4)));
        if (c != base64Digits.npos) result.push_back(std::byte(((b & 15) << 4) | (c >> 2)));
        if (d != base64Digits.npos) result.push_back(std::byte(((c & 3) << 6) | d));
    }
    return result;
}
inline json::Value parameterSnapshotField(const Parameter& field, double value,
                                          bool hidden = false, bool readOnly = false,
                                          const ParameterPresentation* presented = nullptr) {
    using V = json::Value;
    V::Array choices;
    for (const auto& choice : field.choices) choices.emplace_back(choice);
    V::Object result{{"key", V(field.key)}, {"name", V(field.name)},
        {"unit", V(field.unit)}, {"minimum", V(field.minimum)}, {"maximum", V(field.maximum)},
        {"default", V(field.defaultValue)}, {"step", V(field.step)}, {"value", V(value)},
        {"choices", V(std::move(choices))}, {"text", V(field.text(value))},
        {"hidden", V(hidden)}, {"readonly", V(readOnly)}};
    if (presented && (!presented->displayCodes.empty() || !presented->detentLabels.empty())) {
        V::Array codes, detents;
        for (const auto& code : presented->displayCodes) codes.emplace_back(code);
        for (const auto& label : presented->detentLabels) detents.emplace_back(label);
        result.emplace("display", V(V::Object{{"codes", V(std::move(codes))}, {"detents", V(std::move(detents))}}));
    }
    return V(std::move(result));
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
        std::function<std::optional<ParameterPresentation>(ParamId)> presentation{};
        const EditorStatusSource* status{};
        WritableProgramMemory* writablePrograms{};
        std::function<ActionResult(std::string_view, ActionPhase, std::string_view)> actionResult{};
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
            },
            [&engine](ParamId id) -> std::optional<ParameterPresentation> {
                const auto* source = dynamic_cast<const ParameterPresentationSource*>(&engine.processor());
                if (!source) return {};
                return resolveParameterPresentation(*engine.parameters().find(id), source);
            },
            dynamic_cast<const EditorStatusSource*>(&engine.processor()),
            exposePrograms ? dynamic_cast<WritableProgramMemory*>(&engine.processor()) : nullptr,
            [&engine](std::string_view key, ActionPhase phase, std::string_view argument) {
                auto* target = dynamic_cast<ActionTarget*>(&engine.processor());
                return target ? target->webActionWithArgument(engine, key, phase, argument) : ActionResult{};
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
            const auto value = json::read(message, json::Limits{65536, 8192, 65536, 16});
            const auto type = value["type"].text();
            if (type == "subscribe-parameter-deltas" || (deltas_ && type == "parameter-resync")) {
                deltas_ = true;
                resetParameterUpdates();
                return false; // Protocol negotiation never marks the session dirty.
            }
            if (type == "recall-program") return recall(value);
            if (type == "action") return applyAction(value);
            if (type == "store-program" || type == "rename-program" || type == "initialize-program")
                return writeProgram(value, type);
            if (type == "open-file") {
                openFile(value);
                return false;
            }
            const auto key = value["key"].text();
            const auto id = stableId(key);
            const auto binding = allowed_.find(id);
            if (binding == allowed_.end() || binding->second->key != key) return false;
            const auto gesture = active_.find(id);
            if (type == "end" || type == "momentary-release") {
                if (gesture == active_.end() || gesture->second != (type == "momentary-release") ||
                    !endpoint_.end(id)) return false;
                active_.erase(gesture);
                return true;
            }
            const auto presented = endpoint_.presentation ? endpoint_.presentation(id) : std::nullopt;
            if (readOnly_.contains(id) || (presented && (presented->hidden || presented->readOnly))) return false;
            const auto& storage = *binding->second;
            const auto& field = presented ? presented->field : storage;
            const auto toStorage = [&](double number) { return mapParameterValue(number, field, storage); };
            if (type == "begin" || type == "momentary-press") {
                const bool momentary = type == "momentary-press";
                if (momentary && (!endpoint_.beginMomentary || field.minimum >= field.maximum)) return false;
                const auto [entry, inserted] = active_.emplace(id, momentary);
                if (!inserted) return entry->second == momentary;
                try {
                    if (momentary ? endpoint_.beginMomentary(id, toStorage(field.maximum), toStorage(field.minimum)) : endpoint_.begin(id))
                        return true;
                } catch (...) {
                    active_.erase(entry);
                    throw;
                }
                active_.erase(entry);
                return false;
            }
            if (type != "parameter" || (gesture != active_.end() && gesture->second)) return false;
            const auto* number = value["value"].number();
            if (!number || !std::isfinite(*number) || *number < field.minimum || *number > field.maximum ||
                std::abs(field.clamp(*number) - *number) > std::max(1e-9, field.step * 1e-6)) return false;
            return endpoint_.write(id, toStorage(*number), gesture != active_.end());
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
                if (invokeAction(*it, ActionPhase::release, {}).ok) {
                    it = activeActions_.erase(it);
                    continue;
                }
            } catch (...) {}
            ++it;
        }
    }

    std::size_t activeGestures() const noexcept { return active_.size(); }
    std::size_t activeActions() const noexcept { return activeActions_.size(); }
    std::string takeReply() { return std::exchange(reply_, {}); }
    void setFileEndpoint(std::vector<std::string> allowed, std::function<bool(EditorFileRequest)> open) {
        fileExtensions_ = std::move(allowed);
        openFile_ = std::move(open);
    }
    void finishFile(bool cancelled, std::string_view name, std::span<const std::byte> bytes,
                    std::string_view error = {}) {
        if (!fileRequest_) return;
        using V = json::Value;
        const auto request = std::exchange(fileRequest_, std::nullopt);
        if (bytes.size() > maxEditorFileBytes) { bytes = {}; error = "Selected file exceeds 45 KiB"; }
        if (name.size() > 255) { name = {}; error = "Selected filename is too long"; }
        const bool ok = !cancelled && error.empty();
        reply_ = json::write(V(V::Object{{"type", V("file-result")}, {"request", V(*request)},
            {"ok", V(ok)}, {"cancelled", V(cancelled)}, {"name", V(ok ? std::string(name) : std::string{})},
            {"bytes", V(ok ? detail::encodeFileBytes(bytes) : std::string{})},
            {"message", V(std::string(error.substr(0, 512)))}}));
    }

    bool allowsParameter(std::string_view key, bool writable) const {
        const auto id = stableId(key);
        const auto field = allowed_.find(id);
        return field != allowed_.end() && field->second->key == key && (!writable || !readOnly_.contains(id));
    }
    bool allowsAction(std::string_view key, ActionMode mode) const {
        return allowedActions_.contains({std::string(key), mode});
    }

    std::string snapshot() const { return encodeSnapshot(json::Value(snapshotValue())); }
    void advertiseParameterDeltas() noexcept { advertiseDeltas_ = true; }
    void advertiseHostShortcuts() noexcept { hostShortcuts_ = true; }
    bool usesParameterDeltas() const noexcept { return deltas_; }
    void resetParameterUpdates() noexcept { lastSnapshot_.reset(); }
    void resetPage() noexcept {
        deltas_ = false;
        catalogRevision_.reset();
        resetParameterUpdates();
        reply_.clear();
        fileRequest_.reset();
    }

    // Main thread. Advance the baseline only after the view accepts delivery.
    template<class Deliver> bool updateParameters(Deliver deliver) {
        using V = json::Value;
        auto current = snapshotValue();
        if (lastSnapshot_ && *lastSnapshot_ == current) return true;
        const auto nextRevision = ++revision_;
        V message;
        if (lastSnapshot_) {
            V::Object delta{{"type", V("parameter-delta")}, {"base", V(committedRevision_)}};
            const auto& present = *current.at("parameters").array();
            const auto& previous = *lastSnapshot_->at("parameters").array();
            std::map<std::string_view, const V*> old;
            for (const auto& parameter : previous) old.emplace(parameter["key"].text(), &parameter);
            V::Array updated, removed;
            for (const auto& parameter : present) {
                const auto previous = old.find(parameter["key"].text());
                if (previous == old.end() || *previous->second != parameter) updated.push_back(parameter);
                if (previous != old.end()) old.erase(previous);
            }
            for (const auto& [key, field] : old) removed.emplace_back(std::string(key));
            delta.emplace("parameters", V(std::move(updated)));
            delta.emplace("removed", V(std::move(removed)));
            for (const auto& [key, value] : current) {
                if (key == "type" || key == "parameters") continue;
                const auto previous = lastSnapshot_->find(key);
                if (previous == lastSnapshot_->end() || previous->second != value) delta.emplace(key, value);
            }
            for (const auto& [key, value] : *lastSnapshot_)
                if (!current.contains(key)) delta.emplace(key, V{});
            delta.emplace("revision", V(nextRevision));
            message = V(std::move(delta));
        } else {
            current.emplace("revision", V(nextRevision));
            message = V(current);
            current.erase("revision");
        }
        if (!deliver(encodeSnapshot(message))) return false;
        lastSnapshot_ = std::move(current);
        committedRevision_ = nextRevision;
        return true;
    }

private:
    static std::string encodeSnapshot(const json::Value& value) {
        const auto text = json::write(value);
        if (text.size() > 65536) throw std::length_error("Web parameter snapshot exceeds 64 KiB");
        return text;
    }

    json::Value::Object snapshotValue() const {
        using V = json::Value;
        V::Array parameters;
        for (const auto& [id, storage] : allowed_) {
            const auto presented = endpoint_.presentation ? endpoint_.presentation(id) : std::nullopt;
            if (presented && presented->hidden) continue;
            const auto& field = presented ? presented->field : *storage;
            parameters.push_back(detail::parameterSnapshotField(field, mapParameterValue(endpoint_.read(id), *storage, field),
                false, readOnly_.contains(id) || (presented && presented->readOnly), presented ? &*presented : nullptr));
        }
        V::Object snapshot{{"type", V("parameters")}, {"parameters", V(std::move(parameters))}};
        if (advertiseDeltas_) snapshot.emplace("supportsDeltas", V(true));
        if (hostShortcuts_) snapshot.emplace("hostShortcuts", V(true));
        if (openFile_) {
            V::Array extensions;
            for (const auto& extension : fileExtensions_) extensions.emplace_back(extension);
            snapshot.emplace("fileExtensions", V(std::move(extensions)));
        }
        if (endpoint_.beginMomentary) snapshot.emplace("momentary", V(true));
        V::Array actions;
        if (endpoint_.supportsAction)
            for (const auto& [key, mode] : allowedActions_)
                if (endpoint_.supportsAction(key, mode))
                    actions.emplace_back(V::Object{{"key", V(key)}, {"mode", V(mode == ActionMode::invoke ? "invoke" : "momentary")}});
        if (!actions.empty()) snapshot.emplace("actions", V(std::move(actions)));
        if (endpoint_.writablePrograms) {
            const auto revision = endpoint_.writablePrograms->catalogRevision();
            if (catalogRevision_ != revision) {
                const auto programs = endpoint_.programs ? endpoint_.programs() : std::nullopt;
                catalog_ = programs ? detail::programSnapshot(*programs) : V{};
                catalogRevision_ = revision;
            }
            if (!catalog_.isNull()) snapshot.emplace("programs", catalog_);
            snapshot.emplace("writablePrograms", V(true));
            snapshot.emplace("catalogRevision", V(revision));
        } else if (endpoint_.programs) {
            if (const auto programs = endpoint_.programs())
                snapshot.emplace("programs", detail::programSnapshot(*programs));
        }
        if (endpoint_.status) {
            const auto revision = endpoint_.status->editorStatusRevision();
            if (statusRevision_ != revision) {
                V::Array status;
                const auto entries = endpoint_.status->editorStatus();
                if (entries.size() > 64) throw std::length_error("Too many web editor status entries");
                for (const auto& entry : entries) {
                    if (!detail::validActionKey(entry.key) || entry.label.size() > 256 || entry.text.size() > 2048 ||
                        entry.severity.size() > 32)
                        throw std::invalid_argument("Invalid web editor status entry");
                    status.emplace_back(V::Object{{"key", V(entry.key)}, {"label", V(entry.label)},
                        {"text", V(entry.text)}, {"severity", V(entry.severity)}});
                }
                status_ = V(std::move(status));
                statusRevision_ = revision;
            }
            snapshot.emplace("status", status_);
            snapshot.emplace("statusRevision", V(*statusRevision_));
        }
        return snapshot;
    }

    ActionResult invokeAction(std::string_view key, ActionPhase phase, std::string_view argument) {
        if (endpoint_.actionResult) return endpoint_.actionResult(key, phase, argument);
        if (!argument.empty()) return {false, "This action takes no argument"};
        return {endpoint_.action(key, phase), {}};
    }

    void openFile(const json::Value& message) {
        const auto request = message["request"].text();
        if (!openFile_ || request.empty() || request.size() > 80 || fileRequest_) return;
        EditorFileRequest file;
        file.save = message["save"].boolean() && *message["save"].boolean();
        const auto* filters = message["filters"].array();
        if (!filters || filters->empty() || filters->size() > fileExtensions_.size()) return;
        for (const auto& filter : *filters) {
            const auto value = filter.text();
            if (std::ranges::find(fileExtensions_, value) == fileExtensions_.end()) return;
            file.filters.emplace_back(value);
        }
        if (file.save) {
            if (!message["bytes"].isString()) return;
            auto bytes = detail::decodeFileBytes(message["bytes"].text());
            if (!bytes) return;
            file.bytes = std::move(*bytes);
        }
        fileRequest_ = std::string(request);
        try {
            if (openFile_(std::move(file))) return;
        } catch (...) {}
        finishFile(false, {}, {}, "Could not open the system file chooser");
    }

    void reply(std::string_view type, std::string_view key, std::string_view request, bool ok, std::string message) {
        using V = json::Value;
        if (message.size() > 2048) message = "The result message exceeded its size limit";
        reply_ = json::write(V(V::Object{{"type", V(std::string(type))}, {"key", V(std::string(key))},
            {"request", V(std::string(request))}, {"ok", V(ok)}, {"message", V(std::move(message))}}));
    }

    bool applyAction(const json::Value& message) {
        const auto key = message["key"].text();
        const auto phase = message["phase"].text();
        const auto argument = message["argument"].text();
        const auto request = message["request"].text();
        if (!endpoint_.action || !detail::validActionKey(key) || argument.size() > 4096 || request.size() > 80 ||
            (!message["argument"].isNull() && !message["argument"].isString())) return false;
        const auto respond = [&](ActionResult result) {
            reply("action-result", key, request, result.ok, std::move(result.message));
            return result.ok;
        };
        if (phase == "release") {
            if (!argument.empty()) return false;
            const auto active = activeActions_.find(key);
            if (active == activeActions_.end()) return false;
            auto result = invokeAction(key, ActionPhase::release, {});
            if (result.ok) activeActions_.erase(active);
            return respond(std::move(result));
        }
        if (phase != "invoke" && phase != "press") return false;
        const bool momentary = phase == "press";
        const auto mode = momentary ? ActionMode::momentary : ActionMode::invoke;
        if (!allowedActions_.contains({std::string(key), mode}) || !endpoint_.supportsAction(key, mode)) return false;
        if (!momentary) return respond(invokeAction(key, ActionPhase::invoke, argument));
        const auto [active, inserted] = activeActions_.emplace(key);
        if (!inserted) return true;
        ActionResult result;
        try { result = invokeAction(key, ActionPhase::press, argument); }
        catch (...) { activeActions_.erase(active); throw; }
        if (!result.ok) activeActions_.erase(active);
        return respond(std::move(result));
    }

    static std::optional<ProgramAddress> addressOf(const json::Value& message) {
        const auto* bank = message["bank"].unsignedInteger();
        const auto* program = message["program"].unsignedInteger();
        if (!bank || !program || *bank > 16383 || *program > 127) return {};
        return ProgramAddress{static_cast<unsigned>(*bank), static_cast<unsigned>(*program)};
    }

    bool writeProgram(const json::Value& message, std::string_view command) {
        auto* memory = endpoint_.writablePrograms;
        const auto address = addressOf(message);
        const auto request = message["request"].text();
        if (!memory || !address || request.size() > 80 || !active_.empty()) return false;
        ProgramWriteResult result;
        if (command == "initialize-program") {
            result = memory->initialize(*address);
        } else if (command == "rename-program") {
            const auto name = message["name"].text();
            if (name.empty() || name.size() > 80 || std::ranges::any_of(name, [](unsigned char c) { return c < 32 || c == 127; }))
                return false;
            result = memory->rename(*address, name);
        } else {
            const auto* values = message["bytes"].array();
            if (!values || values->size() > 4096) return false;
            std::vector<std::byte> bytes;
            bytes.reserve(values->size());
            for (const auto& item : *values) {
                const auto* value = item.unsignedInteger();
                if (!value || *value > 255) return false;
                bytes.push_back(static_cast<std::byte>(*value));
            }
            result = memory->store(*address, bytes);
        }
        reply("program-result", command, request, result.ok, std::move(result.error));
        if (result.ok) catalogRevision_.reset();
        return result.ok;
    }

    bool recall(const json::Value& message) {
        if (!endpoint_.programs || !active_.empty()) return false;
        const auto address = addressOf(message);
        if (!address) return false;
        const auto programs = endpoint_.programs();
        if (!programs) return false;
        detail::validateProgramCatalog(*programs);
        if (!detail::findProgram(*programs, *address) || !endpoint_.recallProgram(*address)) return false;
        catalogRevision_.reset();
        return true;
    }

    Endpoint endpoint_;
    mutable std::optional<std::uint64_t> statusRevision_;
    mutable json::Value status_;
    mutable std::optional<std::uint64_t> catalogRevision_;
    mutable json::Value catalog_;
    std::string reply_;
    bool deltas_{}, advertiseDeltas_{};
    bool hostShortcuts_{};
    std::uint64_t revision_{}, committedRevision_{};
    std::optional<json::Value::Object> lastSnapshot_;
    std::vector<std::string> fileExtensions_;
    std::function<bool(EditorFileRequest)> openFile_;
    std::optional<std::string> fileRequest_;
    std::map<ParamId, const Parameter*> allowed_;
    std::set<ParamId> readOnly_;
    std::map<ParamId, bool> active_;
    std::set<std::pair<std::string, ActionMode>> allowedActions_;
    std::set<std::string, std::less<>> activeActions_;
};
} // namespace cell::web
