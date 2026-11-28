#pragma once
#include <cstddef>
#include <map>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <variant>
#include <vector>
#include <cstdint>
#include <limits>
#include <type_traits>
#include <cell/text.hpp>

namespace cell::json {

// Owning, bounded, non-realtime JSON. No product or inference-engine dependency.
// Numbers retain exact64-bit integer storage as well as finite doubles.
struct Limits {
    std::size_t inputBytes = 8u * 1024 * 1024;
    std::size_t nodes = 262144;
    std::size_t textBytes = 8u * 1024 * 1024;
    std::size_t depth = 48;
    // The caller already owns this value graph. Large arrays/text are governed
    // by representation/memory; the recursive writer's structural guard stays.
    static Limits forOwnedOutput(){return {std::string{}.max_size(),std::numeric_limits<std::size_t>::max(),std::string{}.max_size(),256};}
};
class Error : public std::runtime_error {
public:
    Error(std::size_t offset, const char* message) : std::runtime_error(message), offset_(offset) {}
    std::size_t byteOffset() const noexcept { return offset_; }
private:
    std::size_t offset_{};
};

class Value {
public:
    using Array = std::vector<Value>;
    using Object = std::map<std::string, Value, std::less<>>;
    Value() = default;
    Value(std::nullptr_t) {}
    explicit Value(bool value) : value_(value) {}
    explicit Value(double value) : value_(Number{value,value}) {}
    template<class T>requires(std::is_integral_v<T>&&!std::is_same_v<T,bool>)
    explicit Value(T value):value_(Number{[] (T x)->std::variant<std::int64_t,std::uint64_t,double>{if constexpr(std::is_signed_v<T>)return std::int64_t(x);else return std::uint64_t(x);}(value),double(value)}){}
    explicit Value(std::string value) : value_(std::move(value)) {}
    explicit Value(const Text&value):value_(value.toStdString()){}
    explicit Value(const char* value) : value_(std::string(value)) {}
    explicit Value(Array value) : value_(std::move(value)) {}
    explicit Value(Object value) : value_(std::move(value)) {}
    bool operator==(const Value&) const;
    bool isNull() const noexcept { return std::holds_alternative<std::nullptr_t>(value_); }
    bool isNumber() const noexcept { return std::holds_alternative<Number>(value_); }
    bool isBool() const noexcept { return std::holds_alternative<bool>(value_); }
    bool isString() const noexcept { return std::holds_alternative<std::string>(value_); }
    bool isObject() const noexcept { return std::holds_alternative<Object>(value_); }
    // Existing consumers can observe a floating value; identities should use
    // the exact integer accessors when present.
    const double* number() const noexcept {const auto*p=std::get_if<Number>(&value_);return p?&p->approximate:nullptr;}
    const std::int64_t* signedInteger()const noexcept{const auto*p=std::get_if<Number>(&value_);return p?std::get_if<std::int64_t>(&p->exact):nullptr;}
    const std::uint64_t* unsignedInteger()const noexcept{const auto*p=std::get_if<Number>(&value_);return p?std::get_if<std::uint64_t>(&p->exact):nullptr;}
    const bool* boolean() const noexcept { return std::get_if<bool>(&value_); }
    const std::string* string() const noexcept { return std::get_if<std::string>(&value_); }
    const Array* array() const noexcept { return std::get_if<Array>(&value_); }
    const Object* object() const noexcept { return std::get_if<Object>(&value_); }
    // Missing/non-object properties return null, without allocating or coercion.
    const Value& operator[](std::string_view) const noexcept;
    std::string_view text() const noexcept { const auto* value = string(); return value ? std::string_view(*value) : std::string_view(); }
private:
    struct Number{std::variant<std::int64_t,std::uint64_t,double>exact;double approximate;bool operator==(const Number&) const = default;};
    std::variant<std::nullptr_t, bool, Number, std::string, Array, Object> value_{nullptr};
};

// Strict JSON: rejects duplicate decoded keys, malformed UTF-8/surrogates,
// trailing data, non-finite numbers and exhausted budgets. Accepts a UTF-8 BOM.
Value read(std::string_view, const Limits& = {});
// Stable object-key order, locale-independent numbers, bounded output. A
// programmatically built invalid/non-finite Value is rejected before return.
std::string write(const Value&, const Limits& = {});
}
