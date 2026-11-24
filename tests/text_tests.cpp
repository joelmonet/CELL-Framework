// Original Cell Framework implementation; see LICENSE.
#include <cell/text_list.hpp>
#include <iostream>
#include <set>
namespace {void need(bool value,const char*message){if(!value)throw std::runtime_error(message);}}
int main(){try{
    const cell::Text vocal=cell::Text::fromUTF8(u8"é声🎙�");
    const auto utf8=std::basic_string(u8"é声🎙�");
    const auto utf8View=std::basic_string_view(utf8.data(),utf8.size());
    need(cell::Text(utf8)==vocal&&cell::Text(utf8View)==vocal&&cell::Text(u8"é声🎙�")==vocal,"UTF-8 constructors depend on native code-unit compiler mode");
    need(cell::Text::fromUTF8(utf8.data(),2)==cell::Text::fromUTF8(u8"é")&&cell::Text::fromUTF8(static_cast<decltype(utf8.data())>(nullptr)).isEmpty(),"UTF-8 byte count or null input changed across compiler modes");
    const auto embedded=std::basic_string_view(u8"a\0声",5);
    need(cell::Text(embedded).getNumBytesAsUTF8()==5&&cell::Text(embedded).length()==3,"UTF-8 string view lost embedded zero or multibyte codepoint");
    need(vocal.length()==4&&vocal[0]==U'é'&&vocal[1]==U'声'&&vocal[2]==U'🎙'&&vocal[3]==0xfffd,"UTF-8 codepoints or valid replacement character corrupted");
    cell::Text rebuilt;for(auto code:vocal)rebuilt+=code;need(rebuilt==vocal,"UTF-8 iteration changed bytes");
    need(cell::Text("Straße").equalsIgnoreCase("STRASSE")&&cell::Text("École Σ").equalsIgnoreCase("éCOLE ς")&&cell::Text("İ").toLowerCase()==cell::Text::fromUTF8(u8"i̇"),"Unicode case folding lost expansions or non-Latin case pairs");
    need(cell::Text("İ/KK/END/file").upToFirstOccurrenceOf("end",false,true)=="İ/KK/"&&cell::Text("Straße-tail").fromFirstOccurrenceOf("STRASSE",false,true)=="-tail","Folded byte offsets corrupted source text");
    need(vocal.substring(1,3)==cell::Text::fromUTF8(u8"声🎙")&&vocal.substring(-100,1)==cell::Text::fromUTF8(u8"é"),"Substring split a codepoint");
    const cell::Text bad(std::string("\xff\xc0\x80\xed\xa0\x80",6));std::size_t invalid{};for(auto code:bad){need(code==0xfffd,"Invalid UTF-8 exposed an invalid scalar");++invalid;}need(invalid==6,"Invalid UTF-8 iteration did not advance safely");
    need(cell::Text("Live 🎙.WAV").matchesWildcard("*?.wav",true)&&cell::Text("é声").matchesWildcard("??")&&!cell::Text("é声").matchesWildcard("?"),"Wildcard matching split Unicode or lost trailing stars");
    need(cell::Text("abc").matchesWildcard("a*")&&cell::Text("abc").matchesWildcard("*b*")&&!cell::Text("abc").matchesWildcard("*bd"),"Wildcard backtracking failed");
    need(cell::Text("*écho.WÁV").matchesWildcard("*É*.wáv",true),"Literal stars or accented filename filters failed");
    need(cell::Text(UINT64_MAX)=="18446744073709551615"&&cell::Text(INT64_MIN)=="-9223372036854775808"&&cell::Text::toHexString(UINT64_MAX)=="ffffffffffffffff","Large native text numbers lost precision");
    need(cell::Text(0.125)=="0.125"&&cell::Text(1.5,2)=="1.50"&&cell::Text(1.,3,true).contains("e+00"),"Native numeric display formatting failed");
    const auto wide=cell::Text(std::numeric_limits<double>::max(),30);need(wide.length()>300&&wide.getDoubleValue()==std::numeric_limits<double>::max(),"Fixed-format numbers were silently lost in a small buffer");
    auto lines=cell::TextList::fromLines("one\r\ntwo\n\nthree\r");need(lines.size()==5&&lines[0]=="one"&&lines[2].isEmpty()&&lines[3]=="three","Mixed line endings changed text data");
    auto query=cell::TextList::fromTokens("snare \"wide room\" '' ride"," \t","\"'");need(query.size()==4&&query[1]=="wide room"&&query[2].isEmpty(),"Quoted search phrases or empty tokens lost");
    query.removeEmptyStrings();query.add("RIDE");query.removeDuplicates(true);need(query.size()==3&&query.joinIntoString("|")=="snare|wide room|ride","Text list deduplication or join failed");
    cell::TextList takes{"Take10","take2","Take1","Take01","Take999999999999999999999999999999"};takes.sortNatural();need(takes[0]=="Take1"&&takes[1]=="Take01"&&takes[2]=="take2"&&takes[3]=="Take10","Natural sorting failed or narrowed a long numeric field");
    cell::TextList large;large.ensureStorageAllocated(100000);for(int i=0;i<100000;++i)large.add(cell::Text(i));need(large.size()==100000&&large[99999]=="99999","Text list has a fixed admission limit");
    std::cout<<"PASS native text/list: Unicode iteration, invalid byte safety, wildcard filters, full numeric range, quoted phrases, line endings and100k entries\n";return 0;
}catch(const std::exception&error){std::cerr<<error.what()<<'\n';return 1;}}
