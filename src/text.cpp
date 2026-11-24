// Original Cell Framework implementation; see LICENSE.
#include <cell/text.hpp>
#include <cell/unicode.hpp>
#include "unicode_case_data.hpp"
#include <vector>

#ifdef CELL_USE_FAST_FLOAT
#include <fast_float/fast_float.h>
#endif

std::from_chars_result cell::detail::parseDoubleChars(
    const char* first, const char* last, double& value) noexcept {
#ifdef CELL_USE_FAST_FLOAT
    const auto parsed = fast_float::from_chars(first, last, value);
    return {parsed.ptr, parsed.ec};
#else
    return std::from_chars(first, last, value);
#endif
}

namespace cell::unicode {
namespace {
template<std::size_t N>CaseMapping lookup(char32_t code,const data::Entry(&entries)[N])noexcept{
    const auto*entry=std::lower_bound(std::begin(entries),std::end(entries),code,[](const auto&item,char32_t key){return item.source<key;});
    if(entry==std::end(entries)||entry->source!=code)return {{code,0,0},1};
    return {{entry->target[0],entry->target[1],entry->target[2]},entry->count};
}
}
CaseMapping mapCase(char32_t code,Case mode)noexcept{
    if(code<128){if((mode==Case::lower||mode==Case::fold)&&code>='A'&&code<='Z')code+='a'-'A';else if(mode==Case::upper&&code>='a'&&code<='z')code-='a'-'A';return {{code,0,0},1};}
    switch(mode){case Case::lower:return lookup(code,data::lower);case Case::upper:return lookup(code,data::upper);case Case::fold:return lookup(code,data::fold);}return {{code,0,0},1};
}
char32_t foldSingle(char32_t code)noexcept{const auto mapping=mapCase(code,Case::fold);return mapping.count==1?mapping.code[0]:code;}
bool isWordCharacter(char32_t code)noexcept{const auto*range=std::lower_bound(std::begin(data::word),std::end(data::word),code,[](const auto&entry,char32_t value){return entry.last<value;});return range!=std::end(data::word)&&code>=range->first;}
}
namespace cell {
namespace {
Text transform(const Text&text,unicode::Case mode){Text result;result.preallocateBytes(text.getNumBytesAsUTF8());for(auto code:text){const auto mapping=unicode::mapCase(code,mode);for(unsigned char at=0;at<mapping.count;++at)result+=mapping.code[at];}return result;}
struct Folded {
    Text::Codepoints at,end;unicode::CaseMapping mapping{};unsigned char offset{};bool fold;
    explicit Folded(const Text&text,bool foldCase=true):at(text.begin()),end(text.end()),fold(foldCase){nextMapping();}
    void nextMapping()noexcept{offset=0;mapping=at==end?unicode::CaseMapping{}:fold?unicode::mapCase(*at,unicode::Case::fold):unicode::CaseMapping{{*at,0,0},1};if(at!=end)++at;}
    bool empty()const noexcept{return !mapping.count;}
    char32_t value()const noexcept{return mapping.code[offset];}
    void advance()noexcept{if(++offset==mapping.count)nextMapping();}
};
std::pair<std::size_t,std::size_t> occurrence(const Text&text,const Text&needle,bool ignoreCase,bool last){
    if(needle.isEmpty()){const auto at=last?text.getNumBytesAsUTF8():0;return {at,at};}
    if(!ignoreCase){const auto at=last?text.utf8().rfind(needle.utf8()):text.utf8().find(needle.utf8());return {at,at==std::string::npos?at:at+needle.getNumBytesAsUTF8()};}
    // Keep source-byte spans when folding expands/shrinks a codepoint. A folded
    // byte offset must never be used directly to cut the original UTF-8 text.
    Text folded;std::vector<std::pair<std::size_t,std::size_t>>source;
    for(auto at=text.begin();at!=text.end();){const auto first=at.bytePosition();const auto mapping=unicode::mapCase(*at,unicode::Case::fold);++at;for(unsigned char i=0;i<mapping.count;++i){const auto encoded=Text::charToString(mapping.code[i]);folded+=encoded;source.insert(source.end(),encoded.getNumBytesAsUTF8(),{first,at.bytePosition()});}}
    const auto match=needle.caseFolded();const auto at=last?folded.utf8().rfind(match.utf8()):folded.utf8().find(match.utf8());
    return at==std::string::npos?std::pair{at,at}:std::pair{source[at].first,source[at+match.getNumBytesAsUTF8()-1].second};
}
}
Text Text::toLowerCase()const{return transform(*this,unicode::Case::lower);}
Text Text::toUpperCase()const{return transform(*this,unicode::Case::upper);}
Text Text::caseFolded()const{return transform(*this,unicode::Case::fold);}
int Text::compareIgnoreCase(const Text&other)const noexcept{Folded a(*this),b(other);while(!a.empty()&&!b.empty()){if(a.value()!=b.value())return a.value()<b.value()?-1:1;a.advance();b.advance();}return a.empty()==b.empty()?0:a.empty()?-1:1;}
int Text::compareNatural(const Text&other,bool ignoreCase)const noexcept{
    Folded a(*this,ignoreCase),b(other,ignoreCase);
    const auto digit=[](const Folded&value){return !value.empty()&&value.value()>='0'&&value.value()<='9';};
    while(!a.empty()&&!b.empty()){
        if(digit(a)&&digit(b)){
            auto an=a,bn=b;std::size_t az{},bz{},ac{},bc{};
            while(digit(an)&&an.value()=='0'){++az;an.advance();}while(digit(bn)&&bn.value()=='0'){++bz;bn.advance();}
            const auto ad=an,bd=bn;while(digit(an)){++ac;an.advance();}while(digit(bn)){++bc;bn.advance();}
            if(ac!=bc)return ac<bc?-1:1;
            auto ap=ad,bp=bd;for(std::size_t i=0;i<ac;++i){if(ap.value()!=bp.value())return ap.value()<bp.value()?-1:1;ap.advance();bp.advance();}
            if(az!=bz)return az<bz?-1:1;
            a=an;b=bn;continue;
        }
        if(a.value()!=b.value())return a.value()<b.value()?-1:1;a.advance();b.advance();
    }
    return a.empty()==b.empty()?0:a.empty()?-1:1;
}
Text Text::upToOccurrence(const Text&needle,bool include,bool ignoreCase,bool last)const{const auto at=occurrence(*this,needle,ignoreCase,last);return at.first==std::string::npos?*this:Text(utf8().substr(0,include?at.second:at.first));}
Text Text::afterOccurrence(const Text&needle,bool include,bool ignoreCase,bool last)const{const auto at=occurrence(*this,needle,ignoreCase,last);return at.first==std::string::npos?Text{}:Text(utf8().substr(include?at.first:at.second));}
}
