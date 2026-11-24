// Original Cell Framework implementation; see LICENSE.
#include <cell/state_tree.hpp>
#include <algorithm>
#include <charconv>
#include <stdexcept>
#include <cell/files.hpp>
#include <fstream>

namespace cell::state {
namespace {
template<class Integer> Integer parseInteger(const Text& text){
    const auto raw=text.trim();auto input=raw.utf8();
    if(input.starts_with('+'))input.remove_prefix(1);
    Integer result{};
    const auto parsed=std::from_chars(input.data(),input.data()+input.size(),result);
    if(parsed.ec==std::errc::result_out_of_range)throw std::out_of_range("State integer is outside its representation");
    return parsed.ec==std::errc{}?result:Integer{};
}
template<class Integer> Integer checkedNumber(double value){
    // Exclusive upper bound avoids the rounded INT64_MAX/UINT64_MAX double.
    const auto upper=std::ldexp(1.0,std::numeric_limits<Integer>::digits);
    const auto minimum=std::is_signed_v<Integer>?-upper:0.0;
    if(!std::isfinite(value)||value<minimum||value>=upper)
        throw std::out_of_range("State number cannot be represented as an integer");
    return static_cast<Integer>(value);
}
bool validName(std::string_view text){
    if(text.empty())return false;
    for(std::size_t i=0;i<text.size();++i){const auto c=static_cast<unsigned char>(text[i]);
        if(c>127||c=='_'||c==':'||(c>='a'&&c<='z')||(c>='A'&&c<='Z'))continue;
        if(i&&(c=='.'||c=='-'||(c>='0'&&c<='9')))continue;
        return false;
    }
    return true;
}
void attribute(std::string& output,std::string_view input){xml::detail::appendAttribute(output,input);}

}
Text Scalar::toString() const{
    return std::visit([](const auto& value)->Text{
        using T=std::decay_t<decltype(value)>;
        if constexpr(std::is_same_v<T,std::monostate>)return {};
        else if constexpr(std::is_same_v<T,Text>)return value;
        else if constexpr(std::is_same_v<T,std::shared_ptr<const ByteBlock>>)return value?value->toBase64Encoding():Text{};
        else if constexpr(std::is_same_v<T,bool>)return value?"1":"0";
        else if constexpr(std::is_integral_v<T>)return Text(value);
        else {
            char buffer[64];const auto result=std::to_chars(buffer,buffer+sizeof(buffer),value,
                std::chars_format::general,std::numeric_limits<double>::max_digits10);
            if(result.ec!=std::errc{})throw std::runtime_error("State number formatting failed");
            return Text(std::string_view(buffer,std::size_t(result.ptr-buffer)));
        }
    },value_);
}
std::int64_t Scalar::integer() const{
    return std::visit([](const auto& value)->std::int64_t{
        using T=std::decay_t<decltype(value)>;
        if constexpr(std::is_same_v<T,std::monostate>)return 0;
        else if constexpr(std::is_same_v<T,Text>)return parseInteger<std::int64_t>(value);
        else if constexpr(std::is_same_v<T,std::shared_ptr<const ByteBlock>>)return 0;
        else if constexpr(std::is_same_v<T,double>)return checkedNumber<std::int64_t>(value);
        else if constexpr(std::is_same_v<T,std::uint64_t>){if(value>std::uint64_t(INT64_MAX))throw std::out_of_range("State unsigned integer exceeds signed representation");return std::int64_t(value);}
        else return std::int64_t(value);
    },value_);
}
std::uint64_t Scalar::unsignedInteger() const{
    return std::visit([](const auto& value)->std::uint64_t{
        using T=std::decay_t<decltype(value)>;
        if constexpr(std::is_same_v<T,std::monostate>)return 0;
        else if constexpr(std::is_same_v<T,Text>)return parseInteger<std::uint64_t>(value);
        else if constexpr(std::is_same_v<T,std::shared_ptr<const ByteBlock>>)return 0;
        else if constexpr(std::is_same_v<T,double>)return checkedNumber<std::uint64_t>(value);
        else if constexpr(std::is_same_v<T,std::int64_t>){if(value<0)throw std::out_of_range("State integer is negative");return std::uint64_t(value);}
        else return std::uint64_t(value);
    },value_);
}
Scalar::operator int() const{const auto value=integer();if(value<INT32_MIN||value>INT32_MAX)throw std::out_of_range("State integer exceeds int representation");return int(value);}
double Scalar::number() const{
    return std::visit([](const auto& value)->double{
        using T=std::decay_t<decltype(value)>;
        if constexpr(std::is_same_v<T,std::monostate>)return 0;
        else if constexpr(std::is_same_v<T,Text>)return value.getDoubleValue();
        else if constexpr(std::is_same_v<T,std::shared_ptr<const ByteBlock>>)return 0;
        else return double(value);
    },value_);
}
Scalar::operator bool() const{
    return std::visit([](const auto& value)->bool{
        using T=std::decay_t<decltype(value)>;
        if constexpr(std::is_same_v<T,std::monostate>)return false;
        else if constexpr(std::is_same_v<T,Text>){const auto text=value.trim();return text.equalsIgnoreCase("true")||text.equalsIgnoreCase("yes")||text.getLargeIntValue()!=0;}
        else if constexpr(std::is_same_v<T,std::shared_ptr<const ByteBlock>>)return value&&value->getSize()!=0;
        else return value!=0;
    },value_);
}
struct Tree::Node {
    Text type;std::weak_ptr<Node> parent;
    std::vector<std::pair<Text,Scalar>> properties;
    std::vector<std::shared_ptr<Node>> children;
    Node* nextDelete{};
    explicit Node(Text value):type(std::move(value)){}
    static void destroy(Node* node)noexcept {
        // Last-reference deletion is iterative and allocation-free even for
        // deeply nested state. Reentrant child releases enqueue their nodes.
        static thread_local Node* pending{};static thread_local bool draining{};
        node->nextDelete=pending;pending=node;if(draining)return;
        draining=true;
        while(pending){auto* next=pending;pending=next->nextDelete;delete next;}
        draining=false;
    }
};
Tree::Tree(Text type){if(!validName(type.utf8()))throw std::invalid_argument("Invalid state node name");node_=std::shared_ptr<Node>(new Node(std::move(type)),&Node::destroy);}
Text Tree::getType() const{return node_?node_->type:Text{};}
bool Tree::hasType(const Text& type) const{return node_&&node_->type==type;}
std::size_t Tree::getNumChildren() const noexcept{return node_?node_->children.size():0;}
std::size_t Tree::getNumProperties() const noexcept{return node_?node_->properties.size():0;}
Tree Tree::getChild(std::size_t index) const{return index<getNumChildren()?Tree(node_->children[index]):Tree{};}
Tree Tree::getParent() const{return node_?Tree(node_->parent.lock()):Tree{};}
Tree Tree::getChildWithName(const Text& name) const{for(auto child:*this)if(child.hasType(name))return child;return {};}
Tree Tree::getChildWithProperty(const Text& key,const Scalar& value) const{for(auto child:*this)if(child.hasProperty(key)){const auto property=child[key];if(property==value||((property.isString()||value.isString())&&property.toString()==value.toString()))return child;}return {};}
bool Tree::hasProperty(const Text& key) const{if(node_)for(const auto& [name,value]:node_->properties)if(name==key)return true;return false;}
Scalar Tree::getProperty(const Text& key,Scalar fallback) const{if(node_)for(const auto& [name,value]:node_->properties)if(name==key)return value;return fallback;}
Text Tree::getPropertyName(std::size_t index) const{return index<getNumProperties()?node_->properties[index].first:Text{};}
void Tree::setProperty(Text name,Scalar value,std::nullptr_t){
    if(!node_)throw std::logic_error("Cannot edit an empty state handle");
    if(!validName(name.utf8()))throw std::invalid_argument("Invalid state property name");
    for(auto& [key,current]:node_->properties)if(key==name){current=std::move(value);return;}
    node_->properties.emplace_back(std::move(name),std::move(value));
}
void Tree::removeProperty(const Text& key,std::nullptr_t){if(node_)std::erase_if(node_->properties,[&](const auto& item){return item.first==key;});}
void Tree::addChild(Tree child,std::ptrdiff_t index,std::nullptr_t){
    if(!node_||!child.node_)throw std::invalid_argument("Cannot parent an empty state handle");
    for(auto ancestor=node_;ancestor;ancestor=ancestor->parent.lock())if(ancestor==child.node_)throw std::invalid_argument("A state node cannot be its own ancestor");
    auto& children=node_->children;
    // Reserve before detaching; failed admission leaves the original tree intact.
    if(children.size()==children.max_size())throw std::length_error("State child count exceeds allocator representation");
    if(children.size()==children.capacity())children.reserve(children.size()+std::min(children.max_size()-children.size(),children.size()/2+1));
    if(auto previous=child.getParent();previous.isValid())previous.removeChild(child);
    const auto at=index<0?children.size():std::min(std::size_t(index),children.size());
    children.insert(children.begin()+at,child.node_);child.node_->parent=node_;
}
void Tree::removeChild(std::size_t index,std::nullptr_t){if(index<getNumChildren()){node_->children[index]->parent.reset();node_->children.erase(node_->children.begin()+index);}}
void Tree::removeChild(const Tree& child,std::nullptr_t){if(node_)for(std::size_t index=0;index<node_->children.size();++index)if(node_->children[index]==child.node_){removeChild(index);break;}}
void Tree::removeAllChildren(std::nullptr_t){if(node_){for(auto& child:node_->children)child->parent.reset();node_->children.clear();}}
Tree Tree::createCopy() const{
    if(!node_)return {};
    Tree result(node_->type);std::vector<std::pair<Tree,Tree>> pending{{*this,result}};
    while(!pending.empty()){
        auto [source,destination]=std::move(pending.back());pending.pop_back();
        destination.node_->properties=source.node_->properties;
        destination.node_->children.reserve(source.getNumChildren());
        for(const auto child:source){Tree copy(child.getType());destination.node_->children.push_back(copy.node_);copy.node_->parent=destination.node_;pending.emplace_back(child,copy);}
    }
    return result;
}
std::string Tree::toXmlString() const{
    if(!node_)return {};
    struct Visit{Tree node;std::size_t child{};bool opened{};};
    std::vector<Visit> pending{{*this}};std::string output;
    while(!pending.empty()){
        auto& visit=pending.back();
        if(!visit.opened){
            output+='<';output+=visit.node.getType().utf8();
            for(const auto& [name,value]:visit.node.node_->properties){output+=' ';if(value.isBinary())output+="base64:";output+=name.utf8();output+="=\"";attribute(output,value.toString().utf8());output+='"';}
            if(!visit.node.getNumChildren()){output+="/>";pending.pop_back();continue;}
            output+='>';visit.opened=true;
        }
        if(visit.child<visit.node.getNumChildren()){
            const auto child=visit.node.getChild(visit.child++);pending.push_back({child});
        }else{output+="</";output+=visit.node.getType().utf8();output+='>';pending.pop_back();}
    }
    return output;
}
Tree Tree::fromXml(std::string_view input){return fromXml(input,legacy::Limits::forInput(input.size()));}
bool Tree::writeXmlFile(const std::filesystem::path&path)const{try{const auto text=toXmlString();files::atomicWrite(path,std::as_bytes(std::span(text.data(),text.size())),true);return true;}catch(const std::exception&){return false;}}
Tree Tree::readXmlFile(const std::filesystem::path&path){try{const auto size=std::filesystem::file_size(path);if(size>std::string{}.max_size())return {};std::ifstream input(path,std::ios::binary);if(!input)return {};std::string text(std::size_t(size),'\0');input.read(text.data(),std::streamsize(size));if(!input&&input.gcount()!=std::streamsize(size))return {};return fromXml(std::string_view(text));}catch(const std::exception&){return {};}}
Tree Tree::fromXml(std::string_view input,const legacy::Limits& limits){
    auto document=legacy::readXml(input,{},limits);
    auto parsed=std::move(document).takeNodes();
    std::vector<Tree> nodes;nodes.reserve(parsed.size());
    for(auto& node:parsed){
        Tree tree(Text(std::move(node.name)));
        for(auto& property:node.attributes){
            if(property.name.starts_with("base64:")){
                ByteBlock bytes;
                if(bytes.fromBase64Encoding(Text(property.value))){
                    const auto name=Text(property.name).substring(7);
                    if(tree.hasProperty(name))throw std::invalid_argument("Duplicate decoded state property");
                    tree.setProperty(name,Scalar(std::move(bytes)));continue;
                }
            }
            if(tree.hasProperty(Text(property.name)))throw std::invalid_argument("Duplicate state property");
            tree.setProperty(Text(std::move(property.name)),Scalar(Text(std::move(property.value))));
        }
        std::vector<legacy::Attribute>{}.swap(node.attributes);
        if(!Text(node.text).trim().isEmpty())throw std::invalid_argument("A state tree cannot discard XML text content");
        nodes.push_back(std::move(tree));
    }
    // Reverse parent order links each subtree after its descendants. Attaching
    // unparented nodes then takes constant time, including very deep projects.
    for(auto index=nodes.size();index>0;--index){auto& children=parsed[index-1].children;for(const auto child:children)nodes[index-1].addChild(nodes[child]);std::vector<legacy::NodeId>{}.swap(children);}
    return nodes.front();
}
} // namespace cell::state
