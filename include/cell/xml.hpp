// Original Cell Framework implementation; see LICENSE.
#pragma once
#include <cell/state_tree.hpp>
#include <filesystem>

namespace cell::xml {
// Attribute/element XML used by project state. Text-bearing XML is rejected
// rather than silently discarding or rearranging mixed content.
class Element {
    friend struct Document;
    Text tag_;
    std::vector<std::pair<Text,Text>> attributes_;
    std::vector<std::unique_ptr<Element>> children_;
    Element* parent_{};
public:
    explicit Element(Text tag):tag_(std::move(tag)){if(tag_.isEmpty())throw std::invalid_argument("XML element name is empty");}
    ~Element();
    Element(const Element&);Element&operator=(const Element&);
    const Text& getTagName() const noexcept{return tag_;}
    bool hasTagName(const Text& tag)const noexcept{return tag_==tag;}
    bool hasAttribute(const Text&)const;
    Text getStringAttribute(const Text&,Text fallback={})const;
    int getIntAttribute(const Text&,int fallback=0)const;
    double getDoubleAttribute(const Text&,double fallback=0)const;
    bool getBoolAttribute(const Text&,bool fallback=false)const;
    void setAttribute(Text,const state::Scalar&);
    void removeAttribute(const Text&);
    Element*createNewChildElement(Text);
    void addChildElement(Element*);
    Element*getChildByName(const Text&)const;
    Element*getFirstChildElement()const noexcept;
    Element*getNextElement()const noexcept;
    std::size_t getNumChildElements()const noexcept{return children_.size();}
    void removeChildElement(Element*,bool deleteObject);
    std::unique_ptr<Element>createCopy()const;
    const auto&attributes()const noexcept{return attributes_;}
    class Iterator {
        using Position=std::vector<std::unique_ptr<Element>>::const_iterator;
        Position position_;
    public:
        explicit Iterator(Position position):position_(position){}
        Element*operator*()const{return position_->get();}
        Iterator&operator++(){++position_;return *this;}
        bool operator==(const Iterator&)const=default;
    };
    struct Children {
        const Element&owner;
        Iterator begin()const{return Iterator(owner.children_.begin());}
        Iterator end()const{return Iterator(owner.children_.end());}
    };
    Children getChildIterator()const noexcept{return {*this};}
    Text toString()const;
    bool writeToPath(const std::filesystem::path&)const;
    template<class File>requires requires(const File& file){file.getFullPathName();}
    bool writeTo(const File& file)const{return writeToPath(std::filesystem::u8path(cell::textView(file.getFullPathName())));}
    static std::unique_ptr<Element>fromTree(const state::Tree&);
    state::Tree toTree()const;
};
struct Document {
    static std::unique_ptr<Element>parse(const Text&);
    static std::unique_ptr<Element>parseFile(const std::filesystem::path&);
    template<class File>requires requires(const File& file){file.getFullPathName();}
    static std::unique_ptr<Element>parse(const File& file){return parseFile(std::filesystem::u8path(cell::textView(file.getFullPathName())));}
    template<class String>requires requires(const String& text){text.toRawUTF8();text.getNumBytesAsUTF8();}
    static std::unique_ptr<Element>parse(const String& text){return parse(Text(cell::textView(text)));}
};
std::vector<std::byte>encodeBinary(const Element&);
std::unique_ptr<Element>decodeBinary(std::span<const std::byte>);
namespace detail {void appendAttribute(std::string&,std::string_view);}
} // namespace cell::xml
