// Original Cell Framework implementation; see LICENSE.
#include <cell/state_tree.hpp>
#include <iostream>

namespace {
void need(bool ok,const char* reason){if(!ok)throw std::runtime_error(reason);}
template<class Function>void rejected(Function call,const char* reason){bool failed=false;try{call();}catch(const std::exception&){failed=true;}need(failed,reason);}
}
int main(){try{
    using namespace cell::state;
    Tree root("Session");
    constexpr std::int64_t sample=INT64_C(9007199254740993);
    constexpr auto largest=UINT64_MAX;
    root.setProperty("sample",sample);root.setProperty("id",largest);
    root.setProperty("name","Vocal & keys <take> \"é 🎙\"\nnew line");
    root.setProperty("enabled",true);root.setProperty("disabled",false);
    root.setProperty("position",14400.125000000002);
    auto alias=root;alias.setProperty("shared",12);
    need(int(root["shared"])==12,"Tree handles lost shared model semantics");
    for(int i=0;i<300;++i){Tree track("Track");track.setProperty("number",i);track.setProperty("sourceFrame",sample+i);root.addChild(track);}
    need(root.getNumChildren()==300&&root.getChild(299).getParent()==root,"Tree imposed a channel cap or lost parent identity");
    auto copy=root.createCopy();copy.setProperty("shared",22);copy.getChild(0).setProperty("number",42);
    need(int(root["shared"])==12&&int(root.getChild(0)["number"])==0,"Deep copy changed its source");
    const auto xml=root.toXmlString();const auto restored=Tree::fromXml(xml);
    need(restored.getNumChildren()==300&&std::int64_t(restored["sample"])==sample&&std::uint64_t(restored["id"])==largest,"XML rounded 64-bit sample positions or identifiers");
    need(restored["name"].toString()==root["name"].toString()&&double(restored["position"])==double(root["position"]),"XML changed Unicode/escaping or floating-point precision");
    need(bool(restored["enabled"])&&!bool(restored["disabled"])&&!bool(Scalar("false")),"String boolean restoration is wrong");
    need(!bool(Scalar("0.5"))&&bool(Scalar("YES")),"Legacy string boolean rules changed");
    need(restored.getChildWithProperty("number",299).isValid(),"XML string properties stopped matching integer lookups");
    need(restored.getChildWithName("Track").isValid()&&root.getChildWithProperty("number",299).getNumProperties()==2,"Named/property child lookup lost nodes");
    Tree moved("Moved");auto child=root.getChild(0);moved.addChild(child);
    need(root.getNumChildren()==299&&moved.getNumChildren()==1&&child.getParent()==moved,"Reparent did not detach from original parent");
    rejected([&]{child.addChild(moved);},"Tree accepted an ownership cycle");
    need(child.getParent()==moved&&moved.getNumChildren()==1,"Rejected cycle changed the tree");
    moved.removeChild(child);need(!child.getParent().isValid(),"Removed child retained its parent");
    moved.addChild(child);moved.removeAllChildren();need(!child.getParent().isValid(),"Clear retained a stale parent link");
    rejected([&]{(void)int(Scalar(INT64_MAX));},"Int conversion wrapped an oversized value");
    rejected([&]{(void)std::int64_t(Scalar(0x1p63));},"Rounded signed overflow was admitted");
    rejected([&]{(void)std::uint64_t(Scalar(0x1p64));},"Rounded unsigned overflow was admitted");
    rejected([&]{(void)Tree::fromXml("<Session><Track></Session>");},"Malformed legacy XML was accepted");
    rejected([&]{(void)Tree::fromXml("<Session>content would be lost</Session>");},"Tree silently discarded mixed content");
    cell::Text text("é🎙abcé");const int offset=text.indexOf("abc");
    need(offset==2&&text.substring(offset,offset+3)==cell::Text("abc")&&text.lastIndexOf("é")==5,"UTF-8 search and substring used different index units");
    std::cout<<"PASS native state: exact 64-bit IDs, XML precision/Unicode, 300 channels, alias/deep-copy, reparent/cycle and invalid-state rejection\n";
    return 0;
}catch(const std::exception& e){std::cerr<<e.what()<<'\n';return 1;}}
