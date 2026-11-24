#include <cell/state_migration.hpp>
#include <algorithm>
#include <cmath>
#include <limits>
#include <set>

namespace cell::migration {
namespace {
bool valid(const Field& f) {
    const auto finite = [](float v) { return std::isfinite(v); };
    if (f.legacyId.empty() || f.key.empty() || !finite(f.minimum) || !finite(f.maximum) ||
        f.maximum <= f.minimum || !finite(f.interval) || f.interval < 0 || !finite(f.skew) || f.skew <= 0 ||
        !finite(f.defaultValue) || f.defaultValue < f.minimum || f.defaultValue > f.maximum ||
        !finite(f.maximum - f.minimum) || (f.interval > 0 && !finite((f.maximum - f.minimum) / f.interval))) return false;
    if (f.missingDefault && (!finite(*f.missingDefault) || *f.missingDefault < f.minimum || *f.missingDefault > f.maximum)) return false;
    if (f.missingDefault && f.kind != Kind::float32 && std::floor(*f.missingDefault) != *f.missingDefault) return false;
    switch (f.kind) {
        case Kind::float32: return true;
        case Kind::boolean: return f.minimum == 0 && f.maximum == 1 && (f.defaultValue == 0 || f.defaultValue == 1);
        case Kind::integer: return std::floor(f.minimum) == f.minimum && std::floor(f.maximum) == f.maximum &&
            std::floor(f.defaultValue) == f.defaultValue && std::abs(f.minimum) <= 16777216 && std::abs(f.maximum) <= 16777216;
        case Kind::choice: return f.minimum == 0 && f.maximum <= 65535 && std::floor(f.maximum) == f.maximum &&
            std::floor(f.defaultValue) == f.defaultValue && f.choices.size() == std::size_t(f.maximum) + 1;
    }
    return false;
}
float legal(const Field& f, float input) {
    auto value = std::clamp(input, f.minimum, f.maximum);
    if (f.kind == Kind::integer || f.kind == Kind::choice) {
        const auto low = std::floor(value);
        const auto remainder = value - low;
        // Deterministic nearest-even integer snapping, including negative ties.
        value = low + ((remainder > .5f || (remainder == .5f && std::fmod(std::abs(low), 2.f) == 1)) ? 1.f : 0.f);
        return std::clamp(value, f.minimum, f.maximum);
    }
    const float interval = f.kind == Kind::float32 ? f.interval : 1;
    if (interval > 0) value = f.minimum + interval * std::floor((value - f.minimum) / interval + .5f);
    return std::clamp(value, f.minimum, f.maximum);
}
float restore(const Field& f, double input, float current) {
    if (!std::isfinite(input) || input > double(std::numeric_limits<float>::max()) || input < -double(std::numeric_limits<float>::max()))
        throw legacy::ReadError(legacy::ErrorCode::parameter, 0, "parameter exceeds finite float32 domain");
    const auto stored = static_cast<float>(input);
    const auto close = [](float a, float b) {
        return std::abs(a - b) <= std::max(std::numeric_limits<float>::min(),
            std::numeric_limits<float>::epsilon() * std::max(std::abs(a), std::abs(b)));
    };
    if (close(stored, current)) return current;
    const auto width = f.maximum - f.minimum;
    const auto cycle = [&](float value) {
        auto normalized = std::clamp((legal(f, value) - f.minimum) / width, 0.f, 1.f);
        if (f.skew != 1) normalized = std::pow(normalized, f.skew);
        auto proportion = normalized;
        if (f.skew != 1 && proportion > 0) proportion = std::exp(std::log(proportion) / f.skew);
        return legal(f, f.minimum + width * proportion);
    };
    // The processor stores the first conversion. The reference parameter-state
    // listener reads it back through the normalized API before publishing.
    const auto published = cycle(cycle(stored));
    return close(published, current) ? current : published;
}
}

Schema::Schema(std::string root, std::vector<Field> fields) : root_(std::move(root)), fields_(std::move(fields)) {
    if (root_.empty() || fields_.empty() || fields_.size() > 65536) throw std::invalid_argument("invalid migration schema");
    for (std::size_t n = 0; n < fields_.size(); ++n) {
        const auto& f = fields_[n];
        if (!valid(f) || !keys_.emplace(f.key, n).second || !legacy_.emplace(f.legacyId, n).second)
            throw std::invalid_argument("invalid or duplicate migration field");
        for (const auto& alias : f.aliases)
            if (alias.empty() || !legacy_.emplace(alias, n).second) throw std::invalid_argument("ambiguous migration alias");
    }
}
std::optional<std::size_t> Schema::findLegacy(std::string_view id) const {
    const auto found = legacy_.find(id); return found == legacy_.end() ? std::optional<std::size_t>{} : found->second;
}
std::optional<std::size_t> Schema::findKey(std::string_view id) const {
    const auto found = keys_.find(id); return found == keys_.end() ? std::optional<std::size_t>{} : found->second;
}
std::vector<Value> Schema::defaults() const {
    std::vector<Value> result; result.reserve(fields_.size());
    for (const auto& f : fields_) result.push_back({f.key, f.defaultValue});
    return result;
}
Candidate::Candidate(legacy::Document document, std::vector<Assignment> values,
                     std::vector<legacy::NodeId> unknown, std::vector<Adjustment> adjustments)
    : document_(std::move(document)), assignments_(std::move(values)), unknown_(std::move(unknown)), adjustments_(std::move(adjustments)) {}
Candidate& Candidate::operator=(Candidate other) noexcept {
    std::swap(document_, other.document_); assignments_.swap(other.assignments_);
    unknown_.swap(other.unknown_); adjustments_.swap(other.adjustments_); return *this;
}
std::vector<Value> Candidate::values() const {
    std::vector<Value> result; result.reserve(assignments_.size());
    for (const auto& a : assignments_) result.push_back({a.key, a.plain});
    return result;
}
float Candidate::value(std::string_view key) const {
    for (const auto& a : assignments_) if (a.key == key) return a.plain;
    throw std::out_of_range("unknown migration key");
}

Candidate prepare(const legacy::Document& document, const Schema& schema, std::span<const Value> baseline, MissingFields missing) {
    if (document.root().name != schema.root()) throw legacy::ReadError(legacy::ErrorCode::root, document.root().begin, "state does not match migration root");
    if (baseline.size() != schema.fields().size()) throw std::invalid_argument("migration requires a complete current-value baseline");
    std::vector<Assignment> values(schema.fields().size());
    std::vector<bool> populated(values.size()), assigned(values.size());
    for (const auto& previous : baseline) {
        const auto index = schema.findKey(previous.key);
        if (!index || populated[*index]) throw std::invalid_argument("unknown or duplicate baseline key");
        const auto& f = schema.fields()[*index];
        if (!std::isfinite(previous.plain) || previous.plain < f.minimum || previous.plain > f.maximum ||
            (f.kind != Kind::float32 && std::floor(previous.plain) != previous.plain))
            throw std::invalid_argument("baseline value outside schema");
        populated[*index] = true;
        values[*index] = {f.key, {}, previous.plain, Origin::retained};
    }
    std::set<std::string_view> seen;
    std::vector<legacy::NodeId> unknown;
    std::vector<Adjustment> adjustments;
    for (const auto id : document.root().children) {
        const auto& node = document.node(id);
        const auto key = node.attribute("id");
        if (node.name != "PARAM") {
            if (key && schema.findLegacy(*key)) throw legacy::ReadError(legacy::ErrorCode::parameter, node.begin, "known parameter identity on a non-PARAM node");
            continue;
        }
        if (!key || key->empty() || !seen.insert(*key).second)
            throw legacy::ReadError(legacy::ErrorCode::parameter, node.begin, "missing or duplicate serialized parameter identity");
        const auto index = schema.findLegacy(*key);
        if (!index) { unknown.push_back(id); continue; }
        if (assigned[*index]) throw legacy::ReadError(legacy::ErrorCode::parameter, node.begin, "multiple legacy identities map to one target");
        assigned[*index] = true;
        const auto& field = schema.fields()[*index];
        const auto encoded = node.attribute("value");
        const auto stored = encoded ? legacy::readParameterNumber(*encoded, node.begin) : double(field.missingDefault.value_or(field.defaultValue));
        const auto restored = restore(field, stored, values[*index].plain);
        if (double(restored) != stored) adjustments.push_back({std::string(*key), stored, restored});
        values[*index] = {field.key, std::string(*key), restored, encoded ? Origin::serialized : Origin::missingValueDefault};
    }
    if (missing == MissingFields::resetToDefaults) {
        for (std::size_t n = 0; n < values.size(); ++n) if (!assigned[n]) {
            const auto& field = schema.fields()[n];
            values[n].plain = restore(field, field.missingDefault.value_or(field.defaultValue), values[n].plain);
            values[n].origin = Origin::omittedDefault;
        }
    } else if (missing != MissingFields::retainCurrent) throw std::invalid_argument("invalid missing-field policy");
    return Candidate(document, std::move(values), std::move(unknown), std::move(adjustments));
}

std::vector<Value> baselineSnapshot(const legacy::Document& document, const Schema& schema) {
    if (document.root().name != schema.root()) throw legacy::ReadError(legacy::ErrorCode::root, document.root().begin, "baseline root mismatch");
    auto result = schema.defaults();
    std::vector<bool> seen(result.size());
    std::set<std::string_view> identities;
    for (const auto id : document.root().children) {
        const auto& node = document.node(id);
        if (node.name != "PARAM") continue;
        const auto key = node.attribute("id");
        if (!key || key->empty() || !identities.insert(*key).second) throw std::invalid_argument("missing or duplicate baseline identity");
        const auto index = schema.findLegacy(*key);
        if (!index) continue;
        if (seen[*index]) throw std::invalid_argument("duplicate baseline target");
        const auto encoded = node.attribute("value");
        if (!encoded) throw std::invalid_argument("baseline value missing");
        const auto value = legacy::readParameterNumber(*encoded, node.begin);
        const auto& field = schema.fields()[*index];
        if (value < field.minimum || value > field.maximum ||
            (field.kind != Kind::float32 && std::floor(value) != value)) throw std::invalid_argument("invalid baseline value");
        result[*index].plain = static_cast<float>(value); seen[*index] = true;
    }
    if (std::find(seen.begin(), seen.end(), false) != seen.end()) throw std::invalid_argument("incomplete current-value snapshot");
    return result;
}
} // namespace cell::migration
