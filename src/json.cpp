#include <cell/json.hpp>
#include <charconv>
#include <cmath>
#include <cstdint>
#include <limits>

namespace cell::json {
bool Value::operator==(const Value& other) const {
    // Compare the recursive array and object alternatives after Value is complete.
    return value_ == other.value_;
}

const Value& Value::operator[](std::string_view name) const noexcept {
    static const Value missing;
    if (const auto* values = object()) {
        const auto found = values->find(name);
        if (found != values->end()) return found->second;
    }
    return missing;
}
namespace {
// Consume one Unicode scalar. Continuations, overlong sequences, surrogates
// and code points beyond U+10FFFF are rejected, including in object keys.
std::uint32_t scalar(std::string_view text, std::size_t& position) {
    const auto begin = position;
    const auto first = static_cast<unsigned char>(text[position++]);
    if (first < 0x80) return first;
    unsigned count{}; std::uint32_t value{}, minimum{};
    if (first >= 0xc2 && first <= 0xdf) { count = 1; value = first & 31; minimum = 0x80; }
    else if (first >= 0xe0 && first <= 0xef) { count = 2; value = first & 15; minimum = 0x800; }
    else if (first >= 0xf0 && first <= 0xf4) { count = 3; value = first & 7; minimum = 0x10000; }
    else throw Error(begin, "Invalid JSON UTF-8");
    if (count > text.size() - position) throw Error(begin, "Incomplete JSON UTF-8");
    while (count--) {
        const auto next = static_cast<unsigned char>(text[position++]);
        if ((next & 0xc0) != 0x80) throw Error(begin, "Invalid JSON UTF-8 continuation");
        value = (value << 6) | (next & 63);
    }
    if (value < minimum || value > 0x10ffff || (value >= 0xd800 && value <= 0xdfff))
        throw Error(begin, "Invalid JSON Unicode scalar");
    return value;
}
void utf8(std::string& output, std::uint32_t value) {
    if (value < 0x80) output += char(value);
    else if (value < 0x800) { output += char(0xc0 | (value >> 6)); output += char(0x80 | (value & 63)); }
    else if (value < 0x10000) { output += char(0xe0 | (value >> 12)); output += char(0x80 | ((value >> 6) & 63)); output += char(0x80 | (value & 63)); }
    else { output += char(0xf0 | (value >> 18)); output += char(0x80 | ((value >> 12) & 63)); output += char(0x80 | ((value >> 6) & 63)); output += char(0x80 | (value & 63)); }
}
class Reader {
public:
    Reader(std::string_view input, const Limits& limits) : text_(input), limits_(limits) {
        if (limits.depth > 256) fail("JSON nesting budget exceeds implementation limit");
        if (input.size() > limits.inputBytes) fail("JSON input exceeds byte budget");
        if (text_.starts_with("\xef\xbb\xbf")) at_ = 3;
    }
    Value read() { space(); auto result = value(0); space(); if (at_ != text_.size()) fail("Trailing JSON data"); return result; }
private:
    [[noreturn]] void fail(const char* message) const { throw Error(at_, message); }
    void space() { while (at_ < text_.size() && (text_[at_] == ' ' || text_[at_] == '\t' || text_[at_] == '\r' || text_[at_] == '\n')) ++at_; }
    bool take(char c) { if (at_ < text_.size() && text_[at_] == c) { ++at_; return true; } return false; }
    void literal(std::string_view word) { if (text_.substr(at_, word.size()) != word) fail("Invalid JSON literal"); at_ += word.size(); }
    unsigned hex4() {
        if (text_.size() - at_ < 4) fail("Incomplete JSON Unicode escape");
        unsigned result = 0;
        for (unsigned n = 0; n < 4; ++n) {
            const char c = text_[at_++]; unsigned digit{};
            if (c >= '0' && c <= '9') digit = unsigned(c - '0');
            else if (c >= 'a' && c <= 'f') digit = unsigned(c - 'a' + 10);
            else if (c >= 'A' && c <= 'F') digit = unsigned(c - 'A' + 10);
            else fail("Invalid JSON Unicode escape");
            result = (result << 4) | digit;
        }
        return result;
    }
    std::string string() {
        if (!take('"')) fail("Expected JSON string");
        std::string result;
        while (at_ < text_.size()) {
            const auto first = static_cast<unsigned char>(text_[at_]);
            if (first == '"') { ++at_; return result; }
            const auto previous = result.size();
            if (first < 32) fail("Unescaped JSON control character");
            if (first == '\\') {
                ++at_; if (at_ == text_.size()) fail("Incomplete JSON escape");
                switch (text_[at_++]) {
                    case '"': result += '"'; break; case '\\': result += '\\'; break; case '/': result += '/'; break;
                    case 'b': result += '\b'; break; case 'f': result += '\f'; break; case 'n': result += '\n'; break;
                    case 'r': result += '\r'; break; case 't': result += '\t'; break;
                    case 'u': {
                        auto code = hex4();
                        if (code >= 0xd800 && code <= 0xdbff) {
                            if (!take('\\') || !take('u')) fail("Missing JSON low surrogate");
                            const auto low = hex4(); if (low < 0xdc00 || low > 0xdfff) fail("Invalid JSON low surrogate");
                            code = 0x10000 + ((code - 0xd800) << 10) + low - 0xdc00;
                        } else if (code >= 0xdc00 && code <= 0xdfff) fail("Unpaired JSON low surrogate");
                        utf8(result, code); break;
                    }
                    default: fail("Unknown JSON escape");
                }
            } else {
                const auto begin = at_; scalar(text_, at_); result.append(text_.substr(begin, at_ - begin));
            }
            const auto added = result.size() - previous;
            if (added > limits_.textBytes - textBytes_) fail("JSON decoded text exceeds budget");
            textBytes_ += added;
        }
        fail("Unterminated JSON string");
    }
    Value number() {
        const auto begin = at_; take('-');
        if (take('0')) { if (at_ < text_.size() && text_[at_] >= '0' && text_[at_] <= '9') fail("Leading zero in JSON number"); }
        else { if (at_ == text_.size() || text_[at_] < '1' || text_[at_] > '9') fail("Invalid JSON number"); digits(); }
        if (take('.')) { const auto before = at_; digits(); if (at_ == before) fail("Missing JSON fraction"); }
        if (take('e') || take('E')) { if (!take('+')) take('-'); const auto before = at_; digits(); if (at_ == before) fail("Missing JSON exponent"); }
        const auto token=text_.substr(begin,at_-begin);
        if(token.find_first_of(".eE")==std::string_view::npos&&token!="-0"){
            if(token.starts_with('-')){std::int64_t exact{};const auto parsed=std::from_chars(token.data(),token.data()+token.size(),exact);if(parsed.ec==std::errc{})return Value(exact);}
            else{std::uint64_t exact{};const auto parsed=std::from_chars(token.data(),token.data()+token.size(),exact);if(parsed.ec==std::errc{})return Value(exact);}
        }
        double result{}; const auto end = text_.data() + at_;
        const auto parsed = cell::detail::parseDoubleChars(text_.data() + begin, end, result);
        if (parsed.ec != std::errc() || parsed.ptr != end || !std::isfinite(result)) fail("JSON number is outside finite double range");
        return Value(result);
    }
    void digits() { while (at_ < text_.size() && text_[at_] >= '0' && text_[at_] <= '9') ++at_; }
    Value value(std::size_t depth) {
        if (nodes_ == limits_.nodes) fail("JSON node budget exhausted"); ++nodes_;
        if (at_ == text_.size()) fail("Missing JSON value");
        if (text_[at_] == '"') return Value(string());
        if (take('{')) {
            if (depth >= limits_.depth) fail("JSON nesting limit exceeded");
            Value::Object object; space(); if (take('}')) return Value(std::move(object));
            for (;;) {
                auto key = string(); space(); if (!take(':')) fail("Missing JSON property separator"); space();
                auto child = value(depth + 1);
                if (!object.emplace(std::move(key), std::move(child)).second) fail("Duplicate JSON object key");
                space(); if (take('}')) return Value(std::move(object));
                if (!take(',')) fail("Missing JSON object separator"); space();
            }
        }
        if (take('[')) {
            if (depth >= limits_.depth) fail("JSON nesting limit exceeded");
            Value::Array array; space(); if (take(']')) return Value(std::move(array));
            for (;;) {
                array.push_back(value(depth + 1)); space(); if (take(']')) return Value(std::move(array));
                if (!take(',')) fail("Missing JSON array separator"); space();
            }
        }
        if (text_[at_] == 't') { literal("true"); return Value(true); }
        if (text_[at_] == 'f') { literal("false"); return Value(false); }
        if (text_[at_] == 'n') { literal("null"); return {}; }
        return number();
    }
    std::string_view text_; const Limits& limits_; std::size_t at_{}, nodes_{}, textBytes_{};
};
class Writer {
public:
    explicit Writer(const Limits& limits) : limits_(limits) { if (limits.depth > 256) fail("JSON nesting budget exceeds implementation limit"); }
    std::string write(const Value& value) { append(value, 0); return std::move(output_); }
private:
    [[noreturn]] void fail(const char* message) const { throw Error(output_.size(), message); }
    void text(std::string_view value) {
        if (value.size() > limits_.inputBytes - output_.size()) fail("JSON output exceeds byte budget");
        output_ += value;
    }
    void string(std::string_view value) {
        if (value.size() > limits_.textBytes - textBytes_) fail("JSON decoded text exceeds budget");
        textBytes_ += value.size(); text("\"");
        constexpr char hex[] = "0123456789abcdef";
        for (std::size_t at = 0; at < value.size();) {
            const auto begin = at; const auto code = scalar(value, at);
            if (code == '"') text("\\\""); else if (code == '\\') text("\\\\");
            else if (code < 32) { char escape[] = {'\\', 'u', '0', '0', hex[code >> 4], hex[code & 15]}; text({escape, 6}); }
            else text(value.substr(begin, at - begin));
        }
        text("\"");
    }
    void append(const Value& value, std::size_t depth) {
        if (nodes_ == limits_.nodes) fail("JSON node budget exhausted"); ++nodes_;
        if (value.isNull()) text("null");
        else if (const auto* b = value.boolean()) text(*b ? "true" : "false");
        else if(const auto* number=value.signedInteger()){
            char buffer[32];const auto result=std::to_chars(buffer,buffer+sizeof(buffer),*number);text({buffer,std::size_t(result.ptr-buffer)});
        }else if(const auto* number=value.unsignedInteger()){
            char buffer[32];const auto result=std::to_chars(buffer,buffer+sizeof(buffer),*number);text({buffer,std::size_t(result.ptr-buffer)});
        }else if (const auto* number = value.number()) {
            if (!std::isfinite(*number)) fail("Non-finite JSON number");
            char buffer[64]; const auto result = std::to_chars(buffer, buffer + sizeof(buffer), *number, std::chars_format::general);
            if (result.ec != std::errc()) fail("Could not encode JSON number"); text({buffer, std::size_t(result.ptr - buffer)});
        } else if (const auto* s = value.string()) string(*s);
        else if (const auto* array = value.array()) {
            if (depth >= limits_.depth) fail("JSON nesting limit exceeded");
            text("["); bool first = true; for (const auto& item : *array) { if (!first) text(","); first = false; append(item, depth + 1); } text("]");
        } else if (const auto* object = value.object()) {
            if (depth >= limits_.depth) fail("JSON nesting limit exceeded");
            text("{"); bool first = true; for (const auto& [key, item] : *object) { if (!first) text(","); first = false; string(key); text(":"); append(item, depth + 1); } text("}");
        }
    }
    const Limits& limits_; std::string output_; std::size_t nodes_{}, textBytes_{};
};
}
Value read(std::string_view text, const Limits& limits) { return Reader(text, limits).read(); }
std::string write(const Value& value, const Limits& limits) { return Writer(limits).write(value); }
}
