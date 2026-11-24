#pragma once

#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

namespace cell::legacy {

// All parsing, decoding and migration preparation belongs off the audio thread.
struct Limits {
    std::size_t inputBytes = 128 * 1024 * 1024;
    std::size_t decodedBytes = 128 * 1024 * 1024;
    std::size_t nodes = 262144;
    std::size_t attributes = 1048576;
    std::size_t attributesPerNode = 4096;
    std::size_t depth = 64;
    // A state reader may admit a trusted project according to the actual input
    // extent. These are syntactic upper bounds, not session/channel ceilings.
    static Limits forInput(std::size_t bytes) noexcept;
};

enum class ErrorCode { envelope, truncated, utf8, xml, unsupported, limit, root, parameter, memoryBlock };

class ReadError : public std::runtime_error {
public:
    ReadError(ErrorCode code, std::size_t byteOffset, const char* message);
    ErrorCode code() const noexcept { return code_; }
    std::size_t byteOffset() const noexcept { return offset_; }
private:
    ErrorCode code_;
    std::size_t offset_;
};

using NodeId = std::size_t;

struct Attribute {
    std::string name;
    std::string value;
};

struct Node {
    std::string name;
    std::vector<Attribute> attributes;
    std::vector<NodeId> children;
    // Direct text/CDATA concatenated in encounter order, excluding child text.
    // Use source(node) when mixed-content placement or original spelling matters.
    std::string text;
    std::size_t begin{}, end{};
    std::optional<std::string_view> attribute(std::string_view name) const noexcept;
};

namespace detail { class XmlReader; }

class Document {
public:
    Document(const Document&) = default;
    Document(Document&&) noexcept;
    Document& operator=(const Document&);
    Document& operator=(Document&&) noexcept;
    std::span<const Node> nodes() const noexcept { return nodes_; }
    const Node& root() const { return nodes_.at(0); }
    const Node& node(NodeId id) const { return nodes_.at(id); }
    std::span<const std::byte> original() const noexcept { return bytes_; }
    std::string_view xml() const noexcept;
    std::string_view source(NodeId id) const;
    std::vector<Node> takeNodes() &&;
    bool hasBinaryEnvelope() const noexcept { return xmlStart_ != 0; }
    bool hasVst3PrivateTrailer() const noexcept { return privateStart_ != 0; }
    // Private metadata is retained verbatim. Interpretation is opt-in through
    // the bounded flat-property reader below; reading a document applies nothing.
    std::span<const std::byte> vst3PrivateData() const noexcept;
private:
    Document() = default;
    void swap(Document&) noexcept;
    friend class detail::XmlReader;
    friend Document readBinaryXml(std::span<const std::byte>, std::string_view, const Limits&);
    friend Document readXml(std::string_view, std::string_view, const Limits&);
    std::vector<std::byte> bytes_;
    std::vector<Node> nodes_;
    std::size_t xmlStart_{}, xmlBytes_{};
    std::size_t privateStart_{}, privateBytes_{};
};

// The binary envelope has an 8-byte LE header: magic 0x21324356 and UTF-8
// payload length, followed by XML, optional zero padding and an opaque tagged
// private-data footer. No data is rewritten or applied.
Document readBinaryXml(std::span<const std::byte>, std::string_view expectedRoot = {}, const Limits& = {});
Document readXml(std::string_view, std::string_view expectedRoot = {}, const Limits& = {});

// Non-audio, allocation-free lookup in a flat binary property footer. The
// producer's node label is immaterial. Unknown, nested, duplicate, malformed or
// oversized metadata returns no value and remains available as opaque bytes.
std::optional<bool> privateBoolean(const Document&, std::string_view property) noexcept;

struct ParameterValue { std::string id; double value{}; NodeId source{}; };
double readParameterNumber(std::string_view, std::size_t byteOffset = 0);

// Read only direct PARAM children. Preserve all other nodes in Document.
// No ID hashing, clamping, default injection or product migration is performed.
std::vector<ParameterValue> parameterValues(const Document&, NodeId parent = 0);

// Legacy MemoryBlock text is a decimal byte count, '.', then LSB-first 6-bit
// symbols. It is a different wire format from RFC 4648 base64.
std::vector<std::byte> decodeMemoryBlock(std::string_view, std::size_t maximumBytes = 64 * 1024 * 1024);
std::string encodeMemoryBlock(std::span<const std::byte>, std::size_t maximumBytes = 64 * 1024 * 1024);

} // namespace cell::legacy
