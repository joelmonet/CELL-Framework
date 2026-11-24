#pragma once
#include <cell/legacy_state.hpp>
#include <map>

namespace cell::migration {

enum class Kind { float32, integer, boolean, choice };
struct Field {
    std::string legacyId, key;
    Kind kind = Kind::float32;
    float minimum{}, maximum = 1, interval{}, skew = 1, defaultValue{};
    bool automatable = true;
    std::vector<std::string> aliases;
    std::vector<std::string> choices;
    std::optional<float> missingDefault;
};
struct Value { std::string key; float plain{}; };
enum class Origin { retained, serialized, missingValueDefault, omittedDefault };
enum class MissingFields { resetToDefaults, retainCurrent };
struct Assignment { std::string key, sourceId; float plain{}; Origin origin{}; };
struct Adjustment { std::string id; double serialized{}; float restored{}; };

class Schema {
public:
    Schema(std::string root, std::vector<Field> fields);
    const std::string& root() const noexcept { return root_; }
    std::span<const Field> fields() const noexcept { return fields_; }
    std::optional<std::size_t> findLegacy(std::string_view) const;
    std::optional<std::size_t> findKey(std::string_view) const;
    std::vector<Value> defaults() const;
private:
    std::string root_;
    std::vector<Field> fields_;
    std::map<std::string, std::size_t, std::less<>> legacy_, keys_;
};

// A complete, owning, offline candidate. No parameter bank or audio graph is
// touched. An explicit baseline supplies the current float32 context; use
// schema.defaults() for a fresh load. Reference APVTS omissions reset defaults.
class Candidate {
public:
    Candidate(const Candidate&) = default;
    Candidate(Candidate&&) noexcept = default;
    Candidate& operator=(Candidate other) noexcept;
    const legacy::Document& document() const noexcept { return document_; }
    std::span<const Assignment> assignments() const noexcept { return assignments_; }
    std::span<const legacy::NodeId> unknownParameters() const noexcept { return unknown_; }
    std::span<const Adjustment> adjustments() const noexcept { return adjustments_; }
    std::vector<Value> values() const;
    float value(std::string_view key) const;
private:
    friend Candidate prepare(const legacy::Document&, const Schema&, std::span<const Value>, MissingFields);
    Candidate(legacy::Document, std::vector<Assignment>, std::vector<legacy::NodeId>, std::vector<Adjustment>);
    legacy::Document document_;
    std::vector<Assignment> assignments_;
    std::vector<legacy::NodeId> unknown_;
    std::vector<Adjustment> adjustments_;
};

Candidate prepare(const legacy::Document&, const Schema&, std::span<const Value> baseline,
                  MissingFields = MissingFields::resetToDefaults);
// A current-value snapshot is already denormalized. Validate completeness and
// preserve its float32 values without applying the preset normalization again.
std::vector<Value> baselineSnapshot(const legacy::Document&, const Schema&);

} // namespace cell::migration
