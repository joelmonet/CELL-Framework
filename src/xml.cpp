// Original Cell Framework implementation; see LICENSE.
#include <cell/xml.hpp>
#include <cell/files.hpp>
#include <fstream>

namespace cell::xml {
Element::Element(const Element&source):tag_(source.tag_){auto copy=source.createCopy();attributes_=std::move(copy->attributes_);children_=std::move(copy->children_);for(auto&child:children_)child->parent_=this;}
Element&Element::operator=(const Element&source){if(this==&source)return *this;auto copy=source.createCopy();std::swap(tag_,copy->tag_);attributes_.swap(copy->attributes_);children_.swap(copy->children_);for(auto&child:children_)child->parent_=this;for(auto&child:copy->children_)child->parent_=copy.get();return *this;}
Element::~Element(){
    // Delete descendants after detaching their children. No recursion, extra
    // allocation, or fixed hierarchy depth is needed during teardown.
    auto*cursor=this;
    for(;;){
        if(!cursor->children_.empty()){
            auto*next=cursor->children_.back().release();cursor->children_.pop_back();cursor=next;
        }else{
            if(cursor==this)break;
            auto*parent=cursor->parent_;cursor->parent_=nullptr;delete cursor;cursor=parent;
        }
    }
}
bool Element::hasAttribute(const Text&key)const{for(const auto&[name,value]:attributes_)if(name==key)return true;return false;}
Text Element::getStringAttribute(const Text&key,Text fallback)const{for(const auto&[name,value]:attributes_)if(name==key)return value;return fallback;}
int Element::getIntAttribute(const Text&key,int fallback)const{return hasAttribute(key)?getStringAttribute(key).getIntValue():fallback;}
double Element::getDoubleAttribute(const Text&key,double fallback)const{return hasAttribute(key)?getStringAttribute(key).getDoubleValue():fallback;}
bool Element::getBoolAttribute(const Text&key,bool fallback)const{return hasAttribute(key)?bool(state::Scalar(getStringAttribute(key))):fallback;}
void Element::setAttribute(Text key,const state::Scalar&value){for(auto&[name,current]:attributes_)if(name==key){current=value.toString();return;}attributes_.emplace_back(std::move(key),value.toString());}
void Element::removeAttribute(const Text&key){std::erase_if(attributes_,[&](const auto&item){return item.first==key;});}
Element*Element::createNewChildElement(Text tag){auto child=std::make_unique<Element>(std::move(tag));auto*result=child.get();children_.push_back(std::move(child));result->parent_=this;return result;}
void Element::addChildElement(Element*raw){
    if(!raw)return;
    if(raw==this||raw->parent_)throw std::invalid_argument("XML child is already parented");
    for(auto*at=parent_;at;at=at->parent_)if(at==raw)throw std::invalid_argument("XML ownership cycle");
    std::unique_ptr<Element>child(raw);children_.push_back(std::move(child));raw->parent_=this;
}
Element*Element::getChildByName(const Text&tag)const{for(const auto&child:children_)if(child->tag_==tag)return child.get();return nullptr;}
Element*Element::getFirstChildElement()const noexcept{return children_.empty()?nullptr:children_.front().get();}
Element*Element::getNextElement()const noexcept{if(parent_)for(std::size_t i=0;i+1<parent_->children_.size();++i)if(parent_->children_[i].get()==this)return parent_->children_[i+1].get();return nullptr;}
void Element::removeChildElement(Element*child,bool destroy){for(auto at=children_.begin();at!=children_.end();++at)if(at->get()==child){child->parent_=nullptr;if(!destroy)at->release();children_.erase(at);return;}}
std::unique_ptr<Element>Element::createCopy()const{
    auto result=std::make_unique<Element>(tag_);
    std::vector<std::pair<const Element*,Element*>>pending{{this,result.get()}};
    while(!pending.empty()){
        const auto[source,destination]=pending.back();pending.pop_back();destination->attributes_=source->attributes_;
        destination->children_.reserve(source->children_.size());
        for(const auto&child:source->children_){auto*copy=destination->createNewChildElement(child->tag_);pending.emplace_back(child.get(),copy);}
    }
    return result;
}
std::unique_ptr<Element>Element::fromTree(const state::Tree&tree){
    if(!tree.isValid())return {};
    auto result=std::make_unique<Element>(tree.getType());
    std::vector<std::pair<state::Tree,Element*>>pending{{tree,result.get()}};
    while(!pending.empty()){
        const auto[source,destination]=pending.back();pending.pop_back();
        for(std::size_t i=0;i<source.getNumProperties();++i){const auto name=source.getPropertyName(i);const auto value=source[name];destination->setAttribute(value.isBinary()?Text("base64:")+name:name,value.toString());}
        destination->children_.reserve(source.getNumChildren());
        for(const auto child:source){auto*copy=destination->createNewChildElement(child.getType());pending.emplace_back(child,copy);}
    }
    return result;
}
state::Tree Element::toTree()const{
    // Link bottom-up to avoid repeatedly walking a deep ancestor chain.
    struct Pending{const Element*source;state::Tree destination;std::size_t firstChild{},children{};};
    std::vector<Pending>nodes{{this,state::Tree(tag_)}};
    for(std::size_t i=0;i<nodes.size();++i){
        const auto source=nodes[i].source;const auto destination=nodes[i].destination;
        auto target=destination;
        for(const auto&[name,value]:source->attributes_){
            ByteBlock bytes;
            if(name.startsWith("base64:")&&bytes.fromBase64Encoding(value)){
                const auto decoded=name.substring(7);if(target.hasProperty(decoded))throw std::invalid_argument("Duplicate decoded state property");target.setProperty(decoded,state::Scalar(std::move(bytes)));
            }else{if(target.hasProperty(name))throw std::invalid_argument("Duplicate state property");target.setProperty(name,value);}
        }
        nodes[i].firstChild=nodes.size();nodes[i].children=source->children_.size();
        for(const auto&child:source->children_)nodes.push_back({child.get(),state::Tree(child->tag_)});
    }
    // Reverse construction would reverse siblings. Each parent receives its
    // children in document order after descendant linkage is complete.
    for(auto i=nodes.size();i>0;--i){const auto parent=i-1;for(std::size_t child=0;child<nodes[parent].children;++child)nodes[parent].destination.addChild(nodes[nodes[parent].firstChild+child].destination);}
    return nodes.front().destination;
}
namespace detail {
void appendAttribute(std::string& output,std::string_view input){
    for(const auto c:input)switch(c){
        case '&':output+="&amp;";break;case '<':output+="&lt;";break;case '>':output+="&gt;";break;
        case '"':output+="&quot;";break;case '\n':output+="&#10;";break;
        case '\r':output+="&#13;";break;case '\t':output+="&#9;";break;
        default:if(static_cast<unsigned char>(c)<32)throw std::invalid_argument("XML contains a control character");output+=c;
    }
}
}
Text Element::toString()const{
    struct Visit{const Element*element;std::size_t child{};bool opened{};};
    std::vector<Visit>pending{{this}};std::string output;
    while(!pending.empty()){
        auto&visit=pending.back();const auto*element=visit.element;
        if(!visit.opened){
            output+='<';output+=element->tag_.utf8();
            for(const auto&[name,value]:element->attributes_){output+=' ';output+=name.utf8();output+="=\"";detail::appendAttribute(output,value.utf8());output+='"';}
            if(element->children_.empty()){output+="/>";pending.pop_back();continue;}
            output+='>';visit.opened=true;
        }
        if(visit.child<element->children_.size()){const auto*child=element->children_[visit.child++].get();pending.push_back({child});}
        else{output+="</";output+=element->tag_.utf8();output+='>';pending.pop_back();}
    }
    return Text(std::move(output));
}
bool Element::writeToPath(const std::filesystem::path&path)const{try{const auto data=toString();files::atomicWrite(path,std::as_bytes(std::span(data.toRawUTF8(),data.getNumBytesAsUTF8())),true);return true;}catch(const std::exception&){return false;}}
std::unique_ptr<Element>Document::parse(const Text&text){try{
    auto document=legacy::readXml(text.utf8(),{},legacy::Limits::forInput(text.getNumBytesAsUTF8()));
    auto parsed=std::move(document).takeNodes();
    auto result=std::make_unique<Element>(Text(parsed.front().name));
    std::vector<Element*>nodes(parsed.size());nodes[0]=result.get();
    for(std::size_t i=0;i<parsed.size();++i){
        const auto&source=parsed[i];auto*target=nodes[i];
        if(!Text(source.text).trim().isEmpty())throw std::invalid_argument("Project XML contains unsupported text content");
        for(const auto&attribute:source.attributes)target->setAttribute(Text(attribute.name),state::Scalar(Text(attribute.value)));
        target->children_.reserve(source.children.size());
        for(const auto child:source.children)nodes[child]=target->createNewChildElement(Text(parsed[child].name));
    }
    return result;
}catch(const std::exception&){return {};}}
std::unique_ptr<Element>Document::parseFile(const std::filesystem::path&path){try{
    const auto size=std::filesystem::file_size(path);
    if(size>std::string{}.max_size())return {};
    std::ifstream input(path,std::ios::binary);if(!input)return {};
    std::string text(std::size_t(size),'\0');input.read(text.data(),std::streamsize(size));if(!input&&input.gcount()!=std::streamsize(size))return {};
    return parse(Text(std::move(text)));
}catch(const std::exception&){return {};}}
std::vector<std::byte>encodeBinary(const Element&element){
    const auto text=element.toString();if(text.getNumBytesAsUTF8()>UINT32_MAX)throw std::length_error("Host XML envelope exceeds its 32-bit wire length");
    std::vector<std::byte>result(8+text.getNumBytesAsUTF8()+1);
    const auto put=[&](std::size_t offset,std::uint32_t value){for(unsigned i=0;i<4;++i)result[offset+i]=std::byte(value>>(i*8));};
    put(0,0x21324356);put(4,std::uint32_t(text.getNumBytesAsUTF8()));
    std::memcpy(result.data()+8,text.toRawUTF8(),text.getNumBytesAsUTF8());return result;
}
std::unique_ptr<Element>decodeBinary(std::span<const std::byte>bytes){try{
    auto document=legacy::readBinaryXml(bytes,{},legacy::Limits::forInput(bytes.size()));
    return Document::parse(Text(document.xml()));
}catch(const std::exception&){return {};}}
} // namespace cell::xml

namespace cell::state {
std::unique_ptr<xml::Element>Tree::createXml()const{return xml::Element::fromTree(*this);}
Tree Tree::fromXml(const xml::Element&element){return element.toTree();}
}
