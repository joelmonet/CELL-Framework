#include <cell/legacy_state.hpp>
#include <cell/text.hpp>
#include <algorithm>
#include <charconv>
#include <cmath>
#include <cstring>
#include <limits>
#include <set>
#include <utility>

namespace cell::legacy {

Limits Limits::forInput(std::size_t bytes) noexcept {
    Limits result;result.inputBytes=result.decodedBytes=std::max(std::size_t(1),bytes);
    result.nodes=result.depth=bytes/3+1;
    result.attributes=result.attributesPerNode=bytes/4+1;
    return result;
}
std::vector<Node> Document::takeNodes() && {
    std::vector<Node> result;result.swap(nodes_);std::vector<std::byte>{}.swap(bytes_);
    xmlStart_=xmlBytes_=privateStart_=privateBytes_=0;return result;
}

ReadError::ReadError(ErrorCode code, std::size_t offset, const char* message)
    : std::runtime_error(message), code_(code), offset_(offset) {}

void Document::swap(Document& other) noexcept {
    bytes_.swap(other.bytes_); nodes_.swap(other.nodes_);
    std::swap(xmlStart_, other.xmlStart_); std::swap(xmlBytes_, other.xmlBytes_);
    std::swap(privateStart_, other.privateStart_); std::swap(privateBytes_, other.privateBytes_);
}

Document::Document(Document&& other) noexcept { swap(other); }
Document& Document::operator=(const Document& other) {
    if (this != &other) { Document copy(other); swap(copy); }
    return *this;
}
Document& Document::operator=(Document&& other) noexcept {
    if (this != &other) { Document moved(std::move(other)); swap(moved); }
    return *this;
}

std::optional<std::string_view> Node::attribute(std::string_view key) const noexcept {
    for (const auto& property : attributes) if (property.name == key) return property.value;
    return {};
}

std::string_view Document::xml() const noexcept {
    if (bytes_.empty()) return {};
    return {reinterpret_cast<const char*>(bytes_.data()) + xmlStart_, xmlBytes_};
}

std::string_view Document::source(NodeId id) const {
    const auto& item = node(id);
    return {reinterpret_cast<const char*>(bytes_.data()) + item.begin, item.end - item.begin};
}

std::span<const std::byte> Document::vst3PrivateData() const noexcept {
    if (!privateStart_) return {};
    return std::span(bytes_).subspan(privateStart_, privateBytes_);
}

namespace {
[[noreturn]] void fail(ErrorCode code, std::size_t offset, const char* message) {
    throw ReadError(code, offset, message);
}
bool space(char c) { return c == ' ' || c == '\t' || c == '\n' || c == '\r'; }
bool xmlCharacter(std::uint32_t c) {
    return c == 9 || c == 10 || c == 13 || (c >= 0x20 && c <= 0xd7ff) ||
           (c >= 0xe000 && c <= 0xfffd) || (c >= 0x10000 && c <= 0x10ffff);
}
std::uint32_t codepoint(std::string_view data, std::size_t& pos, std::size_t base) {
    const auto start = pos;
    auto bad = [&]() -> void { fail(ErrorCode::utf8, base + start, "invalid UTF-8 in state XML"); };
    if (pos == data.size()) { bad(); return 0; }
    const auto first = static_cast<unsigned char>(data[pos++]);
    if (first < 0x80) return first;
    unsigned rest{};
    std::uint32_t value{}, minimum{};
    if (first >= 0xc2 && first <= 0xdf) { rest = 1; value = first & 31; minimum = 0x80; }
    else if (first >= 0xe0 && first <= 0xef) { rest = 2; value = first & 15; minimum = 0x800; }
    else if (first >= 0xf0 && first <= 0xf4) { rest = 3; value = first & 7; minimum = 0x10000; }
    else { bad(); return 0; }
    if (rest > data.size() - pos) { bad(); return 0; }
    while (rest--) {
        const auto ch = static_cast<unsigned char>(data[pos++]);
        if ((ch & 0xc0) != 0x80) bad();
        value = (value << 6) | (ch & 63);
    }
    if (value < minimum || value > 0x10ffff || (value >= 0xd800 && value <= 0xdfff)) bad();
    return value;
}
bool nameStart(std::uint32_t c) {
    return c == ':' || c == '_' || (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') ||
           (c >= 0xc0 && c <= 0xd6) || (c >= 0xd8 && c <= 0xf6) ||
           (c >= 0xf8 && c <= 0x2ff) || (c >= 0x370 && c <= 0x37d) ||
           (c >= 0x37f && c <= 0x1fff) || (c >= 0x200c && c <= 0x200d) ||
           (c >= 0x2070 && c <= 0x218f) || (c >= 0x2c00 && c <= 0x2fef) ||
           (c >= 0x3001 && c <= 0xd7ff) || (c >= 0xf900 && c <= 0xfdcf) ||
           (c >= 0xfdf0 && c <= 0xfffd) || (c >= 0x10000 && c <= 0xeffff);
}
bool nameCharacter(std::uint32_t c) {
    return nameStart(c) || c == '-' || c == '.' || (c >= '0' && c <= '9') || c == 0xb7 ||
           (c >= 0x300 && c <= 0x36f) || (c >= 0x203f && c <= 0x2040);
}
void checkLimits(const Limits& limits) {
    if (!limits.inputBytes || !limits.decodedBytes || !limits.nodes ||
        !limits.attributes || !limits.attributesPerNode || !limits.depth)
        throw std::invalid_argument("invalid legacy state limits");
}
bool equalAsciiCase(std::string_view a, std::string_view b) {
    if (a.size() != b.size()) return false;
    for (std::size_t i = 0; i < a.size(); ++i) {
        const auto lower = [](char c) { return c >= 'A' && c <= 'Z' ? char(c - 'A' + 'a') : c; };
        if (lower(a[i]) != lower(b[i])) return false;
    }
    return true;
}
}

namespace detail {
class XmlReader {
public:
    XmlReader(Document& document, const Limits& limits) : document_(document), limits_(limits), data_(document.xml()) {}
    void run(std::string_view expectedRoot) {
        for (std::size_t n = 0; n < data_.size();) {
            const auto start = n;
            if (!xmlCharacter(codepoint(data_, n, document_.xmlStart_)))
                fail(ErrorCode::xml, document_.xmlStart_ + start, "forbidden XML character");
        }
        if (data_.starts_with("\xef\xbb\xbf")) pos_ = 3;
        if (starts("<?xml") && pos_ + 5 < data_.size() && space(data_[pos_ + 5])) declaration();
        misc();
        elements();
        misc();
        if (pos_ != data_.size()) error(ErrorCode::xml, "content after XML root");
        if (!expectedRoot.empty() && document_.root().name != expectedRoot)
            fail(ErrorCode::root, document_.root().begin, "unexpected state root type");
    }
private:
    Document& document_;
    const Limits& limits_;
    std::string_view data_;
    std::size_t pos_{}, decoded_{}, attributeCount_{};

    [[noreturn]] void error(ErrorCode code, const char* message) const {
        fail(code, document_.xmlStart_ + pos_, message);
    }
    bool starts(std::string_view token) const { return data_.substr(pos_).starts_with(token); }
    void expect(std::string_view token) {
        if (!starts(token)) error(ErrorCode::xml, "unexpected XML syntax");
        pos_ += token.size();
    }
    bool whitespace() {
        const auto before = pos_;
        while (pos_ < data_.size() && space(data_[pos_])) ++pos_;
        return pos_ != before;
    }
    void charge(std::size_t count) {
        if (count > limits_.decodedBytes - decoded_) error(ErrorCode::limit, "decoded XML exceeds limit");
        decoded_ += count;
    }
    std::string_view name() {
        const auto start = pos_;
        if (pos_ == data_.size()) error(ErrorCode::xml, "missing XML name");
        auto end = pos_;
        if (!nameStart(codepoint(data_, end, document_.xmlStart_))) error(ErrorCode::xml, "invalid XML name");
        pos_ = end;
        while (pos_ < data_.size()) {
            end = pos_;
            if (!nameCharacter(codepoint(data_, end, document_.xmlStart_))) break;
            pos_ = end;
        }
        charge(pos_ - start);
        return data_.substr(start, pos_ - start);
    }
    void append(std::string& out, std::string_view value) { charge(value.size()); out.append(value); }
    void scalar(std::string& out, std::uint32_t value) {
        char encoded[4]; std::size_t count{};
        if (value < 0x80) encoded[count++] = char(value);
        else if (value < 0x800) { encoded[count++] = char(0xc0 | (value >> 6)); encoded[count++] = char(0x80 | (value & 63)); }
        else if (value < 0x10000) { encoded[count++] = char(0xe0 | (value >> 12)); encoded[count++] = char(0x80 | ((value >> 6) & 63)); encoded[count++] = char(0x80 | (value & 63)); }
        else { encoded[count++] = char(0xf0 | (value >> 18)); encoded[count++] = char(0x80 | ((value >> 12) & 63)); encoded[count++] = char(0x80 | ((value >> 6) & 63)); encoded[count++] = char(0x80 | (value & 63)); }
        append(out, {encoded, count});
    }
    void entity(std::string& out) {
        expect("&");
        const auto end = data_.find(';', pos_);
        if (end == std::string_view::npos || end - pos_ > 16) error(ErrorCode::xml, "invalid XML entity");
        const auto token = data_.substr(pos_, end - pos_);
        std::uint32_t value{};
        if (token == "amp") value = '&'; else if (token == "lt") value = '<';
        else if (token == "gt") value = '>'; else if (token == "quot") value = '"';
        else if (token == "apos") value = '\'';
        else if (token.starts_with('#')) {
            auto number = token.substr(1); int radix = 10;
            if (number.starts_with('x')) { number.remove_prefix(1); radix = 16; }
            if (number.empty()) error(ErrorCode::xml, "empty character reference");
            const auto result = std::from_chars(number.data(), number.data() + number.size(), value, radix);
            if (result.ec != std::errc{} || result.ptr != number.data() + number.size() || !xmlCharacter(value))
                error(ErrorCode::xml, "invalid character reference");
        } else error(ErrorCode::unsupported, "custom XML entities are unsupported");
        pos_ = end + 1;
        scalar(out, value);
    }
    std::string attributeValue() {
        if (pos_ == data_.size() || (data_[pos_] != '\'' && data_[pos_] != '"')) error(ErrorCode::xml, "unquoted XML attribute");
        const auto quote = data_[pos_++];
        std::string result;
        while (pos_ < data_.size() && data_[pos_] != quote) {
            const auto ch = data_[pos_];
            if (ch == '<') error(ErrorCode::xml, "literal '<' in attribute");
            if (ch == '&') entity(result);
            else if (space(ch)) {
                ++pos_;
                if (ch == '\r' && pos_ < data_.size() && data_[pos_] == '\n') ++pos_;
                append(result, " ");
            } else {
                const auto begin = pos_++;
                while (pos_ < data_.size() && data_[pos_] != quote && data_[pos_] != '<' && data_[pos_] != '&' && !space(data_[pos_])) ++pos_;
                append(result, data_.substr(begin, pos_ - begin));
            }
        }
        if (pos_ == data_.size()) error(ErrorCode::xml, "unterminated XML attribute");
        ++pos_;
        return result;
    }
    void declaration() {
        expect("<?xml");
        std::vector<Attribute> attributes;
        while (!starts("?>")) {
            if (!whitespace()) error(ErrorCode::xml, "invalid XML declaration");
            if (starts("?>")) break;
            if (attributes.size() == 3) error(ErrorCode::xml, "excess XML declaration attributes");
            auto key = std::string(name()); whitespace(); expect("="); whitespace();
            attributes.push_back({std::move(key), attributeValue()});
        }
        expect("?>");
        if (attributes.empty() || attributes[0].name != "version" || attributes[0].value != "1.0")
            error(ErrorCode::unsupported, "only XML 1.0 is supported");
        bool encoding{}, standalone{};
        for (std::size_t n = 1; n < attributes.size(); ++n) {
            const auto& a = attributes[n];
            if (a.name == "encoding" && !encoding && !standalone && equalAsciiCase(a.value, "utf-8")) encoding = true;
            else if (a.name == "standalone" && !standalone && (a.value == "yes" || a.value == "no")) standalone = true;
            else error(ErrorCode::unsupported, "unsupported XML declaration");
        }
    }
    void comment() {
        expect("<!--");
        const auto end = data_.find("-->", pos_);
        if (end == std::string_view::npos) error(ErrorCode::xml, "unterminated XML comment");
        const auto body = data_.substr(pos_, end - pos_);
        if (body.find("--") != std::string_view::npos || body.ends_with('-')) error(ErrorCode::xml, "invalid XML comment");
        pos_ = end + 3;
    }
    void instruction() {
        expect("<?");
        if (equalAsciiCase(name(), "xml")) error(ErrorCode::xml, "misplaced XML declaration");
        if (!starts("?>") && !whitespace()) error(ErrorCode::xml, "invalid processing instruction");
        const auto end = data_.find("?>", pos_);
        if (end == std::string_view::npos) error(ErrorCode::xml, "unterminated processing instruction");
        pos_ = end + 2;
    }
    void misc() {
        for (;;) {
            whitespace();
            if (starts("<!--")) comment();
            else if (starts("<?")) instruction();
            else if (starts("<!")) error(ErrorCode::unsupported, "DTD/declarations are unsupported");
            else break;
        }
    }
    std::pair<NodeId,bool> beginElement(std::size_t depth) {
        if (depth > limits_.depth || document_.nodes_.size() >= limits_.nodes) error(ErrorCode::limit, "XML tree exceeds limit");
        const auto begin = document_.xmlStart_ + pos_;
        expect("<");
        const auto tag = std::string(name());
        const auto id = document_.nodes_.size();
        document_.nodes_.push_back({tag, {}, {}, {}, begin, 0});
        std::set<std::string_view> seen;
        while (!starts(">") && !starts("/>")) {
            if (!whitespace()) error(ErrorCode::xml, "missing whitespace between attributes");
            if (starts(">") || starts("/>")) break;
            auto& properties = document_.nodes_[id].attributes;
            if (properties.size() >= limits_.attributesPerNode || attributeCount_ >= limits_.attributes)
                error(ErrorCode::limit, "XML attributes exceed limit");
            const auto key = name();
            if (!seen.insert(key).second) error(ErrorCode::xml, "duplicate XML attribute");
            whitespace(); expect("="); whitespace();
            properties.push_back({std::string(key), attributeValue()});
            ++attributeCount_;
        }
        if (starts("/>")) {
            pos_ += 2;document_.nodes_[id].end = document_.xmlStart_ + pos_;return {id,true};
        }
        expect(">");return {id,false};
    }
    void elements() {
        const auto [root,closed]=beginElement(1);
        std::vector<NodeId> stack;
        if(!closed)stack.push_back(root);
        while(!stack.empty()) {
            const auto id=stack.back();
            if(pos_==data_.size())error(ErrorCode::xml,"unterminated XML element");
            if(starts("</")) {
                pos_+=2;if(name()!=document_.nodes_[id].name)error(ErrorCode::xml,"mismatched closing tag");
                whitespace();expect(">");document_.nodes_[id].end=document_.xmlStart_+pos_;stack.pop_back();
            } else if(starts("<!--"))comment();
            else if(starts("<?"))instruction();
            else if(starts("<![CDATA[")) {
                pos_+=9;const auto end=data_.find("]]>",pos_);
                if(end==std::string_view::npos)error(ErrorCode::xml,"unterminated CDATA");
                text(document_.nodes_[id].text,end,false);pos_+=3;
            } else if(starts("<!"))error(ErrorCode::unsupported,"DTD/declarations are unsupported");
            else if(starts("<")) {
                const auto [child,empty]=beginElement(stack.size()+1);
                document_.nodes_[id].children.push_back(child);if(!empty)stack.push_back(child);
            } else {
                const auto end=data_.find('<',pos_);
                text(document_.nodes_[id].text,end==std::string_view::npos?data_.size():end,true);
            }
        }
    }
    void text(std::string& out, std::size_t end, bool entities) {
        while (pos_ < end) {
            if (entities && starts("]]>")) error(ErrorCode::xml, "CDATA terminator outside CDATA");
            const auto ch = data_[pos_];
            if (entities && ch == '&') entity(out);
            else if (ch == '\r') {
                ++pos_;
                if (pos_ < end && data_[pos_] == '\n') ++pos_;
                append(out, "\n");
            } else {
                const auto begin = pos_++;
                while (pos_ < end && data_[pos_] != '\r' && (!entities || data_[pos_] != '&')) {
                    if (entities && data_[pos_] == ']' && starts("]]>")) error(ErrorCode::xml, "CDATA terminator outside CDATA");
                    ++pos_;
                }
                append(out, data_.substr(begin, pos_ - begin));
            }
        }
    }
};
} // namespace detail

Document readBinaryXml(std::span<const std::byte> input, std::string_view expectedRoot, const Limits& limits) {
    checkLimits(limits);
    if (input.size() > limits.inputBytes) fail(ErrorCode::limit, 0, "state input exceeds limit");
    if (input.size() < 8) fail(ErrorCode::truncated, input.size(), "truncated binary XML header");
    auto le = [&](std::size_t offset) {
        std::uint32_t value{};
        for (unsigned n = 0; n < 4; ++n) value |= std::uint32_t(std::to_integer<unsigned char>(input[offset + n])) << (8 * n);
        return value;
    };
    if (le(0) != 0x21324356u) fail(ErrorCode::envelope, 0, "unrecognized binary XML envelope");
    const auto length = std::size_t(le(4));
    if (!length) fail(ErrorCode::envelope, 4, "empty XML payload");
    if (length > input.size() - 8) fail(ErrorCode::truncated, 4, "XML payload shorter than declared length");
    const auto xmlEnd = 8 + length;
    std::size_t privateStart{}, privateBytes{}, paddingEnd = input.size();
    // Private metadata is opaque. Locate its bounded ASCII identifier and
    // preceding length; no producer-specific tag or payload is interpreted.
    const auto identifier=[](unsigned char c){return (c>='A'&&c<='Z')||(c>='a'&&c<='z')||(c>='0'&&c<='9')||c=='_';};
    auto tagStart=input.size();
    while(tagStart>xmlEnd&&identifier(std::to_integer<unsigned char>(input[tagStart-1])))--tagStart;
    if (tagStart<input.size()) {
        if(input.size()-tagStart>64)fail(ErrorCode::envelope,tagStart,"private-data identifier exceeds limit");
        if (tagStart-xmlEnd < 16) fail(ErrorCode::envelope, xmlEnd, "truncated VST3 private-data footer");
        const auto sizeOffset = tagStart - 8;
        std::uint64_t count{};
        for (unsigned n = 0; n < 8; ++n) count |= std::uint64_t(std::to_integer<unsigned char>(input[sizeOffset + n])) << (8 * n);
        if (count > sizeOffset - xmlEnd - 8) fail(ErrorCode::envelope, sizeOffset, "invalid VST3 private-data length");
        privateBytes = std::size_t(count); privateStart = sizeOffset - privateBytes; paddingEnd = privateStart;
    }
    for (std::size_t n = xmlEnd; n < paddingEnd; ++n)
        if (input[n] != std::byte{}) fail(ErrorCode::envelope, n, "nonzero bytes after XML payload");
    Document document;
    document.bytes_.assign(input.begin(), input.end()); document.xmlStart_ = 8; document.xmlBytes_ = length;
    document.privateStart_ = privateStart; document.privateBytes_ = privateBytes;
    detail::XmlReader(document, limits).run(expectedRoot);
    return document;
}

Document readXml(std::string_view xml, std::string_view expectedRoot, const Limits& limits) {
    checkLimits(limits);
    if (xml.size() > limits.inputBytes) fail(ErrorCode::limit, 0, "state input exceeds limit");
    Document document;
    const auto bytes = std::as_bytes(std::span(xml.data(), xml.size()));
    document.bytes_.assign(bytes.begin(), bytes.end()); document.xmlBytes_ = xml.size();
    detail::XmlReader(document, limits).run(expectedRoot);
    return document;
}

std::optional<bool> privateBoolean(const Document& document,std::string_view property) noexcept {
    const auto data=document.vst3PrivateData();
    if(data.empty()||data.size()>65536||property.empty()||property.size()>4096)return {};
    struct Reader {
        std::span<const std::byte> data;std::size_t position{};
        bool number(std::uint32_t& result)noexcept {
            if(position==data.size())return false;
            const auto count=std::to_integer<unsigned>(data[position++]);
            if(count>4||count>data.size()-position)return false;
            result=0;for(unsigned n=0;n<count;++n)result|=std::to_integer<std::uint32_t>(data[position++])<<(n*8);
            return true;
        }
        bool name(std::string_view& result)noexcept {
            const auto begin=position;
            while(position<data.size()&&data[position]!=std::byte{}){
                if(position-begin>=4096)return false;++position;
            }
            if(position==data.size())return false;
            result={reinterpret_cast<const char*>(data.data()+begin),position-begin};++position;return true;
        }
    } reader{data};
    std::string_view label;std::uint32_t count{};
    if(!reader.name(label)||label.empty()||!reader.number(count)||count>4096)return {};
    std::optional<bool> result;
    for(unsigned n=0;n<count;++n){
        std::string_view key;std::uint32_t length{};
        if(!reader.name(key)||key.empty()||!reader.number(length)||length>data.size()-reader.position)return {};
        if(key==property){
            if(result.has_value()||length!=1)return {};
            const auto type=std::to_integer<unsigned>(data[reader.position]);
            if(type!=2&&type!=3)return {};result=type==2;
        }
        reader.position+=length;
    }
    std::uint32_t children{};
    if(!reader.number(children)||children||reader.position!=data.size())return {};
    return result;
}

std::vector<ParameterValue> parameterValues(const Document& document, NodeId parent) {
    std::vector<ParameterValue> result;
    std::set<std::string_view> seen;
    for (const auto id : document.node(parent).children) {
        const auto& node = document.node(id);
        if (node.name != "PARAM") continue;
        const auto key = node.attribute("id"), encoded = node.attribute("value");
        if (!key || key->empty() || !encoded || !seen.insert(*key).second)
            fail(ErrorCode::parameter, node.begin, "missing or duplicate PARAM identity/value");
        result.push_back({std::string(*key), readParameterNumber(*encoded, node.begin), id});
    }
    return result;
}

double readParameterNumber(std::string_view number, std::size_t byteOffset) {
        while (!number.empty() && space(number.front())) number.remove_prefix(1);
        while (!number.empty() && space(number.back())) number.remove_suffix(1);
        if (number.starts_with('+')) {
            number.remove_prefix(1);
            if (number.starts_with('-') || number.starts_with('+'))
                fail(ErrorCode::parameter, byteOffset, "invalid PARAM numeric sign");
        }
        double value{};
        const auto conversion = cell::detail::parseDoubleChars(number.data(), number.data() + number.size(), value);
        if (number.empty() || conversion.ec != std::errc{} || conversion.ptr != number.data() + number.size() || !std::isfinite(value))
            fail(ErrorCode::parameter, byteOffset, "invalid PARAM numeric value");
    return value;
}

std::vector<std::byte> decodeMemoryBlock(std::string_view text, std::size_t maximumBytes) {
    const auto separator = text.find('.');
    if (separator == std::string_view::npos || !separator || separator > 20)
        fail(ErrorCode::memoryBlock, 0, "invalid MemoryBlock size prefix");
    std::size_t size{};
    const auto number = text.substr(0, separator);
    const auto result = std::from_chars(number.data(), number.data() + number.size(), size);
    if (result.ec != std::errc{} || result.ptr != number.data() + number.size())
        fail(ErrorCode::memoryBlock, 0, "invalid MemoryBlock byte count");
    if (size > maximumBytes || size > (std::numeric_limits<std::size_t>::max() - 5) / 8)
        fail(ErrorCode::limit, 0, "MemoryBlock exceeds decode limit");
    const auto body = text.substr(separator + 1);
    if (body.size() != (size * 8 + 5) / 6) fail(ErrorCode::memoryBlock, separator + 1, "MemoryBlock length mismatch");
    // Accumulate symbols into a little-endian bit reservoir, emitting complete
    // bytes. This implementation is independent of the producer's bit-range API.
    constexpr std::string_view alphabet = ".ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+";
    std::vector<std::byte> bytes; bytes.reserve(size);
    unsigned reservoir{}, bits{};
    for (std::size_t n = 0; n < body.size(); ++n) {
        const auto symbol = alphabet.find(body[n]);
        if (symbol == std::string_view::npos) fail(ErrorCode::memoryBlock, separator + 1 + n, "invalid MemoryBlock symbol");
        reservoir |= unsigned(symbol) << bits; bits += 6;
        if (bits >= 8) { bytes.push_back(std::byte(reservoir & 255)); reservoir >>= 8; bits -= 8; }
    }
    if (bytes.size() != size || reservoir != 0) fail(ErrorCode::memoryBlock, text.size(), "invalid MemoryBlock padding bits");
    return bytes;
}

std::string encodeMemoryBlock(std::span<const std::byte> bytes, std::size_t maximumBytes) {
    if(bytes.size()>maximumBytes||bytes.size()>(std::numeric_limits<std::size_t>::max()-5)/8)
        fail(ErrorCode::limit,0,"MemoryBlock exceeds encoding limit");
    constexpr std::string_view alphabet=".ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+";
    auto result=std::to_string(bytes.size())+".";
    result.reserve(result.size()+(bytes.size()*8+5)/6);
    unsigned reservoir{},bits{};
    for(auto byte:bytes){reservoir|=std::to_integer<unsigned>(byte)<<bits;bits+=8;
        while(bits>=6){result+=alphabet[reservoir&63];reservoir>>=6;bits-=6;}}
    if(bits)result+=alphabet[reservoir&63];return result;
}

} // namespace cell::legacy
