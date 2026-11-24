// Original Cell Framework implementation; see LICENSE.
#include <cell/xml.hpp>
#include <cell/result.hpp>
#include <iostream>
#include <chrono>

namespace {
void need(bool ok,const char* reason){if(!ok)throw std::runtime_error(reason);}
}
int main(){try{
    using namespace cell;
    xml::Element root("DrumScenePlugin");root.setAttribute("version",1);
    root.setAttribute("loop",14400.125000000002);root.setAttribute("enabled",false);
    root.setAttribute("name","é 🎙 & <take> \"quote\"\nline");
    auto*map=root.createNewChildElement("DrumMidiMap");map->setAttribute("name","Custom map");
    auto*note=map->createNewChildElement("Note");note->setAttribute("key",38);
    const auto text=root.toString();auto parsed=xml::Document::parse(text);
    need(parsed&&parsed->hasTagName("DrumScenePlugin")&&parsed->getIntAttribute("version")==1,"Native XML root/attributes changed");
    need(parsed->getDoubleAttribute("loop")==root.getDoubleAttribute("loop")&&!parsed->getBoolAttribute("enabled",true),"XML lost numeric precision or false boolean");
    need(parsed->getStringAttribute("name")==root.getStringAttribute("name")&&parsed->getChildByName("DrumMidiMap")->getFirstChildElement()->getIntAttribute("key")==38,"XML lost Unicode, escaping or nested maps");
    const auto binary=xml::encodeBinary(root);auto restored=xml::decodeBinary(binary);
    need(restored&&restored->toString()==text&&binary[0]==std::byte{0x56}&&binary[1]==std::byte{0x43}&&binary[2]==std::byte{0x32}&&binary[3]==std::byte{0x21},"Existing host XML envelope changed");
    auto truncated=binary;truncated.resize(9);need(!xml::decodeBinary(truncated),"Truncated host state was accepted");
    const unsigned char bytes[]{0,1,2,127,128,255};state::Tree tree("MidiClip");tree.setProperty("version",2);
    tree.setProperty("data",state::Scalar(ByteBlock(bytes,sizeof(bytes))));
    const auto blob=tree["data"];const auto copy=tree.createCopy();
    need(copy["data"].getBinaryData()==blob.getBinaryData(),"State cloning copied immutable binary payloads");
    const auto serialized=tree.toXmlString();need(serialized.find("base64:data=")!=std::string::npos,"Binary attribute wire name changed");
    const auto decoded=state::Tree::fromXml(serialized);need(decoded["data"]==blob,"Binary state XML changed MIDI bytes");
    const auto element=tree.createXml();const auto fromElement=state::Tree::fromXml(*element);
    need(fromElement["data"]==blob,"Element/tree conversion lost the binary property");
    ByteBlock payload;need(payload.fromBase64Encoding(blob.toString()),"Legacy binary property encoding could not decode");
    need(!payload.fromBase64Encoding("99999999999999999999.bad")&&payload.getSize()==sizeof(bytes),"Rejected binary input changed prior bytes");
    // Exercise beyond the old 262144-node ceiling without PCM or a giant file.
    std::string wide="<Session>";for(int n=0;n<300000;++n)wide+="<Track/>";wide+="</Session>";
    const auto many=state::Tree::fromXml(wide);need(many.getNumChildren()==300000,"Native state parser retained a fixed node ceiling");
    std::string deep;for(int n=0;n<4999;++n)deep+="<N>";deep+="<N/>";for(int n=0;n<4999;++n)deep+="</N>";
    {auto nested=state::Tree::fromXml(deep);auto clone=nested.createCopy();need(clone.toXmlString()==deep,"Deep state clone/write changed its hierarchy");auto xml=xml::Document::parse(Text(deep));need(xml&&xml->toString()==Text(deep)&&xml->createCopy()->toString()==Text(deep),"Native XML retained a fixed depth ceiling");}
    need(Text("  +42").getIntValue()==42&&Text(" +1.25").getDoubleValue()==1.25,"Native numeric text rejected whitespace/plus");
    need(Text("999999999999999999999").getLargeIntValue()==INT64_MAX,"Integer overflow selected an automatic zero budget");
    need(std::isinf(Text("1e9999").getDoubleValue())&&Text("1e-9999").getDoubleValue()==0,"Numeric overflow/underflow semantics changed");
    need(Result::ok().wasOk()&&Result::fail("failure").failed()&&Result::fail("").failed(),"Native result success/failure is ambiguous");
    need(!xml::Document::parse(Text("<N>&external;</N>"))&&!xml::Document::parse(Text("<N><M></N>")),"Invalid XML was admitted");
    std::cout<<"PASS native project XML: legacy envelope/binary attributes, immutable MIDI blobs, Unicode/precision, 300000 lanes, 5000-level hierarchy, numeric bounds and invalid-state rejection\n";
    return 0;
}catch(const std::exception& error){std::cerr<<error.what()<<'\n';return 1;}}
