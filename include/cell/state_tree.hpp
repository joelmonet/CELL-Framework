// Original Cell Framework implementation; see LICENSE.
#pragma once
#include <cell/text.hpp>
#include <cell/legacy_state.hpp>
#include <cell/byte_block.hpp>
#include <cmath>
#include <cstdint>
#include <memory>
#include <variant>
#include <vector>
#include <filesystem>

namespace cell::xml {class Element;}
namespace cell::state {
// Exact integers are retained independently of floating point. Audio sample
// positions and object identifiers must survive values greater than 2^53.
class Scalar {
    std::variant<std::monostate, bool, std::int64_t, std::uint64_t, double, Text, std::shared_ptr<const ByteBlock>> value_;
public:
    Scalar() = default;
    Scalar(bool value):value_(value){}
    template<class T> requires(std::is_integral_v<T> && !std::is_same_v<T,bool>)
    Scalar(T value) {
        if constexpr(std::is_signed_v<T>)value_=std::int64_t(value);
        else value_=std::uint64_t(value);
    }
    Scalar(double value):value_(value){}
    Scalar(float value):value_(double(value)){}
    Scalar(Text value):value_(std::move(value)){}
    Scalar(const char* value):value_(Text{value}){}
    Scalar(std::string_view value):value_(Text{value}){}
    Scalar(std::string value):value_(Text{std::move(value)}){}
    Scalar(ByteBlock bytes):value_(std::make_shared<const ByteBlock>(std::move(bytes))){}
    template<class T>requires requires(const T& value){value.toRawUTF8();value.getNumBytesAsUTF8();}
    Scalar(const T& value):value_(Text(value)){}
    bool isVoid() const noexcept{return std::holds_alternative<std::monostate>(value_);}
    bool isString() const noexcept{return std::holds_alternative<Text>(value_);}
    bool isBool() const noexcept{return std::holds_alternative<bool>(value_);}
    bool isInt64() const noexcept{return std::holds_alternative<std::int64_t>(value_)||std::holds_alternative<std::uint64_t>(value_);}
    bool isInt() const noexcept{return false;} // Integer storage is explicitly 64-bit.
    bool isDouble() const noexcept{return std::holds_alternative<double>(value_);}
    bool isBinary() const noexcept{return getBinaryData()!=nullptr;}
    const ByteBlock* getBinaryData()const noexcept{const auto*p=std::get_if<std::shared_ptr<const ByteBlock>>(&value_);return p?p->get():nullptr;}
    Text toString() const;
    std::int64_t integer() const;
    std::uint64_t unsignedInteger() const;
    double number() const;
    explicit operator int() const;
    explicit operator std::int64_t() const{return integer();}
    explicit operator std::uint64_t() const{return unsignedInteger();}
    explicit operator double() const{return number();}
    explicit operator float() const{return float(number());}
    explicit operator bool() const;
    bool operator==(const Scalar& other) const {if(isBinary()&&other.isBinary())return *getBinaryData()==*other.getBinaryData();return value_==other.value_;}
};

// Shared handles intentionally alias a mutable control-thread model. Deep copies
// are explicit. Parent links are weak, and adding a child rejects cycles.
class Tree {
    struct Node;
    std::shared_ptr<Node> node_;
    explicit Tree(std::shared_ptr<Node> node):node_(std::move(node)){}
public:
    Tree()=default;
    explicit Tree(Text type);
    bool isValid() const noexcept{return bool(node_);}
    Text getType() const;
    bool hasType(const Text&) const;
    bool operator==(const Tree&) const = default;
    std::size_t getNumChildren() const noexcept;
    std::size_t getNumProperties() const noexcept;
    Tree getChild(std::size_t) const;
    Tree getParent() const;
    Tree getChildWithName(const Text&) const;
    Tree getChildWithProperty(const Text&,const Scalar&) const;
    bool hasProperty(const Text&) const;
    Scalar getProperty(const Text&,Scalar fallback={}) const;
    Scalar operator[](const Text& key) const{return getProperty(key);}
    Text getPropertyName(std::size_t) const;
    void setProperty(Text,Scalar,std::nullptr_t=nullptr);
    void removeProperty(const Text&,std::nullptr_t=nullptr);
    void addChild(Tree,std::ptrdiff_t index=-1,std::nullptr_t=nullptr);
    void removeChild(std::size_t,std::nullptr_t=nullptr);
    void removeChild(const Tree&,std::nullptr_t=nullptr);
    void removeAllChildren(std::nullptr_t=nullptr);
    Tree createCopy() const;
    std::string toXmlString() const;
    std::unique_ptr<xml::Element> createXml() const;
    bool writeXmlFile(const std::filesystem::path&)const;
    static Tree readXmlFile(const std::filesystem::path&);
    static Tree fromXml(std::string_view);
    static Tree fromXml(std::string_view,const legacy::Limits&);
    static Tree fromXml(const xml::Element&);
    template<class T>requires requires(const T& text){text.toRawUTF8();text.getNumBytesAsUTF8();}
    static Tree fromXml(const T& text){return fromXml(cell::textView(text));}
    class Iterator {
        const Tree* tree_{};std::size_t index_{};
    public:
        Iterator(const Tree* tree,std::size_t index):tree_(tree),index_(index){}
        Tree operator*() const{return tree_->getChild(index_);}
        Iterator& operator++(){++index_;return *this;}
        bool operator==(const Iterator&) const=default;
    };
    Iterator begin() const{return {this,0};}
    Iterator end() const{return {this,getNumChildren()};}
};
} // namespace cell::state
#include <cell/xml.hpp>
