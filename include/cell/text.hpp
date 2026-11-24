// Original Cell Framework implementation; see LICENSE.
#pragma once
#include <algorithm>
#include <charconv>
#include <cstdint>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <string>
#include <string_view>
#include <limits>
#include <type_traits>
#include <array>
#include <ostream>
#include <bit>
#include <cstddef>
#include <stdexcept>
#include <utility>
#include <cell/unicode.hpp>

namespace cell {

namespace detail {
std::from_chars_result parseDoubleChars(const char* first, const char* last, double& value) noexcept;
}

template<class T>std::string_view textView(const T& text){
    if constexpr(std::is_convertible_v<const T&,std::string_view>)return std::string_view(text);
    else if constexpr(requires{text.toRawUTF8();text.getNumBytesAsUTF8();})return {text.toRawUTF8(),std::size_t(text.getNumBytesAsUTF8())};
    else return text.utf8();
}
class Text {
    std::string value_;
    static unsigned char fold(unsigned char c)noexcept{return c>='A'&&c<='Z'?static_cast<unsigned char>(c+('a'-'A')):c;}
    std::string_view numberView()const noexcept{auto text=std::string_view(value_);while(!text.empty()&&(text.front()==' '||text.front()=='\t'||text.front()=='\r'||text.front()=='\n'))text.remove_prefix(1);if(text.starts_with('+'))text.remove_prefix(1);return text;}
    std::size_t byteOffset(int characters)const noexcept{if(characters<=0)return 0;auto at=begin();while(at!=end()&&characters-->0)++at;return at.bytePosition();}
    void formatNumber(double value,std::chars_format format,int precision=-1){
        std::array<char,128> initial{};
        auto convert=[&](char*begin,char*end){return precision<0?std::to_chars(begin,end,value,format):std::to_chars(begin,end,value,format,precision);};
        auto result=convert(initial.data(),initial.data()+initial.size());
        if(result.ec==std::errc{}){value_.assign(initial.data(),result.ptr);return;}
        std::string buffer(initial.size()*2,'\0');
        for(;;){result=convert(buffer.data(),buffer.data()+buffer.size());if(result.ec==std::errc{}){buffer.resize(std::size_t(result.ptr-buffer.data()));value_=std::move(buffer);return;}if(result.ec!=std::errc::value_too_large||buffer.size()>buffer.max_size()/2)throw std::length_error("Text number cannot be formatted");buffer.resize(buffer.size()*2);}
    }
    Text upToOccurrence(const Text&,bool include,bool ignoreCase,bool last)const;
    Text afterOccurrence(const Text&,bool include,bool ignoreCase,bool last)const;
  public:
    Text()=default;Text(const char* v):value_(v?v:""){}Text(std::string v):value_(std::move(v)){}Text(std::string_view v):value_(v){}
    template<class T>requires(!std::is_arithmetic_v<T>&&requires(const T&t){t.toRawUTF8();t.getNumBytesAsUTF8();})Text(const T&t):value_(textView(t)){}
    template<class T>requires(std::is_integral_v<T>)explicit Text(T v):value_(std::to_string(v)){}
    template<class Char>requires(std::is_integral_v<Char>&&sizeof(Char)==1&&!std::is_same_v<Char,bool>)
    Text(std::basic_string_view<Char> value):value_(reinterpret_cast<const char*>(value.data()),value.size()){}
    template<class Char,class Traits,class Allocator>requires(std::is_integral_v<Char>&&sizeof(Char)==1&&!std::is_same_v<Char,bool>)
    Text(const std::basic_string<Char,Traits,Allocator>& value):value_(reinterpret_cast<const char*>(value.data()),value.size()){}
    template<class Char>requires(std::is_integral_v<Char>&&sizeof(Char)==1&&!std::is_same_v<Char,bool>)
    Text(const Char* value):Text(fromUTF8(value)){}
    explicit Text(double value){formatNumber(value,std::chars_format::general);}
    Text(double value,int decimals,bool scientific=false){formatNumber(value,scientific?std::chars_format::scientific:std::chars_format::fixed,std::max(0,decimals));}
    static Text fromUTF8(const char*value,std::ptrdiff_t bytes=-1){return value?Text{bytes<0?std::string_view(value):std::string_view(value,std::size_t(bytes))}:Text{};}
    template<class Char>requires(std::is_integral_v<Char>&&sizeof(Char)==1&&!std::is_same_v<Char,bool>)
    static Text fromUTF8(const Char*value,std::ptrdiff_t bytes=-1){return fromUTF8(reinterpret_cast<const char*>(value),bytes);}
    static Text charToString(char32_t code){std::string out;if(code>0x10ffff||(code>=0xd800&&code<=0xdfff))code=0xfffd;if(code<0x80)out+=char(code);else if(code<0x800){out+=char(0xc0|(code>>6));out+=char(0x80|(code&63));}else if(code<0x10000){out+=char(0xe0|(code>>12));out+=char(0x80|((code>>6)&63));out+=char(0x80|(code&63));}else{out+=char(0xf0|(code>>18));out+=char(0x80|((code>>12)&63));out+=char(0x80|((code>>6)&63));out+=char(0x80|(code&63));}return Text{std::move(out)};}
    template<class T>requires std::is_integral_v<T>static Text toHexString(T value){char buffer[2*sizeof(T)];const auto result=std::to_chars(buffer,buffer+sizeof(buffer),std::make_unsigned_t<T>(value),16);return Text{std::string_view(buffer,std::size_t(result.ptr-buffer))};}
    class Codepoints {
        std::string_view text_;std::size_t offset_{};
        std::pair<char32_t,std::size_t> decoded()const noexcept{
            if(offset_>=text_.size())return {0,0};
            const auto first=static_cast<unsigned char>(text_[offset_]);
            if(first<128)return {first,1};
            const int count=first>=0xc2&&first<=0xdf?2:first>=0xe0&&first<=0xef?3:first>=0xf0&&first<=0xf4?4:0;
            if(!count||std::size_t(count)>text_.size()-offset_)return {0xfffd,1};
            char32_t value=first&((1u<<(7-count))-1);
            for(int i=1;i<count;++i){const auto next=static_cast<unsigned char>(text_[offset_+std::size_t(i)]);if((next&0xc0)!=0x80)return {0xfffd,1};value=(value<<6)|(next&63);}
            if(value<(count==2?0x80u:count==3?0x800u:0x10000u)||value>0x10ffff||(value>=0xd800&&value<=0xdfff))return {0xfffd,1};
            return {value,std::size_t(count)};
        }
      public:
        Codepoints(std::string_view text,std::size_t offset):text_(text),offset_(offset){}
        char32_t operator*()const noexcept{return decoded().first;}
        Codepoints&operator++()noexcept{offset_+=decoded().second;return *this;}
        std::size_t bytePosition()const noexcept{return offset_;}
        bool operator==(const Codepoints&other)const noexcept{return offset_==other.offset_&&text_.data()==other.text_.data();}
    };
    Codepoints begin()const noexcept{return {value_,0};}Codepoints end()const noexcept{return {value_,value_.size()};}
    char32_t operator[](int index)const noexcept{return index<0?0:*Codepoints(value_,byteOffset(index));}
    struct Utf8View{const char*data;std::size_t bytes;const char*getAddress()const noexcept{return data;}std::size_t sizeInBytes()const noexcept{return bytes+1;}};
    Utf8View toUTF8()const noexcept{return {value_.c_str(),value_.size()};}
    const std::string&toStdString()const noexcept{return value_;}
    const Text&toString()const noexcept{return *this;}
    std::string_view utf8()const noexcept{return value_;}
    operator std::string_view()const noexcept{return value_;}
    template<class T>requires(requires{typename T::CharPointerType;}&&std::is_constructible_v<T,const char*>)operator T()const{return T(value_.c_str());}
    const char*toRawUTF8()const noexcept{return value_.c_str();}std::size_t getNumBytesAsUTF8()const noexcept{return value_.size();}
    bool isEmpty()const noexcept{return value_.empty();}bool isNotEmpty()const noexcept{return !value_.empty();}
    void clear()noexcept{value_.clear();}
    int length()const noexcept{int result=0;for(auto at=begin();at!=end();++at)++result;return result;}
    bool operator==(const Text&)const=default;bool operator<(const Text&t)const{return value_<t.value_;}
    template<class T>requires requires(const T&t){t.toRawUTF8();t.getNumBytesAsUTF8();}bool operator==(const T&t)const noexcept{return utf8()==textView(t);}
    Text operator+(const Text&t)const{return Text{value_+t.value_};}
    template<class T>requires requires(const T&t){t.toRawUTF8();t.getNumBytesAsUTF8();}Text operator+(const T&t)const{return Text{value_+std::string(textView(t))};}
    template<class T>requires requires(const T&t){t.toRawUTF8();t.getNumBytesAsUTF8();}friend Text operator+(const T&left,const Text&right){return Text{std::string(textView(left))+right.value_};}
    friend Text operator+(const char*a,const Text&b){return Text{a}+b;}
    Text&operator+=(const Text&t){value_+=t.value_;return *this;}
    Text&operator<<(const Text&t){return *this+=t;}
    Text&operator<<(char value){value_+=value;return *this;}
    Text&operator<<(char32_t value){return *this+=charToString(value);}
    template<class T>requires std::is_arithmetic_v<T>Text&operator<<(T value){return *this+=Text(value);}
    Text&operator+=(char32_t value){return *this+=charToString(value);}
    friend std::ostream&operator<<(std::ostream&out,const Text&text){return out.write(text.value_.data(),std::streamsize(text.value_.size()));}
    int compare(const Text&t)const noexcept{return value_.compare(t.value_);}
    int compareIgnoreCase(const Text&)const noexcept;
    int compareNatural(const Text&,bool ignoreCase=true)const noexcept;
    std::uint64_t hashCode64()const noexcept{std::uint64_t result=14695981039346656037ULL;for(unsigned char c:value_){result^=c;result*=1099511628211ULL;}return result;}
    std::int32_t hashCode()const noexcept{return std::bit_cast<std::int32_t>(std::uint32_t(hashCode64()));}
    bool equalsIgnoreCase(const Text&t)const noexcept{return compareIgnoreCase(t)==0;}
    Text toLowerCase()const;Text toUpperCase()const;Text caseFolded()const;
    Text paddedLeft(char pad,int minimum)const{const int count=std::max(0,minimum-length());return Text{std::string(std::size_t(count),pad)+value_};}
    bool startsWithChar(char32_t code)const noexcept{return !isEmpty()&&*begin()==code;}bool endsWithChar(char32_t code)const{return !isEmpty()&&getLastCharacter()==code;}
    char32_t getLastCharacter()const noexcept{if(isEmpty())return 0;auto at=value_.size()-1;while(at&&(static_cast<unsigned char>(value_[at])&0xc0)==0x80)--at;return *Codepoints(value_,at);}
    bool containsChar(char32_t code)const{return indexOfChar(code)>=0;}
    int indexOfChar(char32_t code)const{int at=0;for(auto c:*this){if(c==code)return at;++at;}return -1;}
    int lastIndexOfChar(char32_t code)const{int at=0,result=-1;for(auto c:*this){if(c==code)result=at;++at;}return result;}
    int indexOfAnyOf(const Text&set)const{int at=0;for(auto c:*this){if(set.indexOfChar(c)>=0)return at;++at;}return -1;}
    Text retainCharacters(const Text&set)const{Text result;for(auto c:*this)if(set.indexOfChar(c)>=0)result+=c;return result;}
    Text removeCharacters(const Text&set)const{Text result;for(auto c:*this)if(set.indexOfChar(c)<0)result+=c;return result;}
    Text replaceCharacter(char32_t from,char32_t to)const{Text result;for(auto c:*this)result+=c==from?to:c;return result;}
    void preallocateBytes(std::size_t bytes){value_.reserve(bytes);}
    bool matchesWildcard(const Text&pattern,bool ignoreCase=false)const{
        auto text=begin(),mark=text,glob=pattern.begin(),star=pattern.end();bool starFound=false;
        auto equal=[&](char32_t a,char32_t b){return a==b||(ignoreCase&&unicode::foldSingle(a)==unicode::foldSingle(b));};
        while(text!=end()){
            if(glob!=pattern.end()&&(*glob=='?'||(*glob!='*'&&equal(*text,*glob)))){++text;++glob;}
            else if(glob!=pattern.end()&&*glob=='*'){star=++glob;mark=text;starFound=true;}
            else if(starFound){glob=star;text=++mark;}
            else return false;
        }
        while(glob!=pattern.end()&&*glob=='*')++glob;
        return glob==pattern.end();
    }
    bool containsOnly(const Text&t)const{return value_.find_first_not_of(t.value_)==std::string::npos;}
    std::int64_t getLargeIntValue()const{std::int64_t value{};const auto raw=numberView();const auto r=std::from_chars(raw.data(),raw.data()+raw.size(),value);if(r.ec==std::errc::result_out_of_range)return raw.starts_with('-')?INT64_MIN:INT64_MAX;return r.ec==std::errc{}?value:0;}
    bool contains(const Text&t)const{return value_.find(t.value_)!=std::string::npos;}
    bool containsIgnoreCase(const Text&t)const{return caseFolded().contains(t.caseFolded());}
    bool startsWith(const Text&t)const{return value_.starts_with(t.value_);}bool endsWith(const Text&t)const{return value_.ends_with(t.value_);}
    bool startsWithIgnoreCase(const Text&t)const{return caseFolded().startsWith(t.caseFolded());}bool endsWithIgnoreCase(const Text&t)const{return caseFolded().endsWith(t.caseFolded());}
    int indexOf(const Text&t)const{auto p=value_.find(t.value_);return p==std::string::npos?-1:int(std::count_if(value_.begin(),value_.begin()+p,[](char c){return (static_cast<unsigned char>(c)&0xc0)!=0x80;}));}
    int lastIndexOf(const Text&t)const{auto p=value_.rfind(t.value_);return p==std::string::npos?-1:int(std::count_if(value_.begin(),value_.begin()+p,[](char c){return (static_cast<unsigned char>(c)&0xc0)!=0x80;}));}
    Text substring(int from,int end=std::numeric_limits<int>::max())const{const auto a=byteOffset(from),b=std::max(a,byteOffset(end));return Text{value_.substr(a,b-a)};}
    Text dropLastCharacters(int count)const{return substring(0,std::max(0,length()-std::max(0,count)));}
    Text trimStart()const{const auto start=value_.find_first_not_of(" \t\r\n");return start==std::string::npos?Text{}:Text{value_.substr(start)};}
    Text trim()const{const auto a=value_.find_first_not_of(" \t\r\n"),b=value_.find_last_not_of(" \t\r\n");return a==std::string::npos?Text{}:Text{value_.substr(a,b-a+1)};}
    Text trimEnd()const{const auto end=value_.find_last_not_of(" \t\r\n");return end==std::string::npos?Text{}:Text{value_.substr(0,end+1)};}
    Text trimCharactersAtEnd(const Text& chars)const{const auto end=value_.find_last_not_of(chars.value_);return end==std::string::npos?Text{}:Text{value_.substr(0,end+1)};}
    Text replace(const Text&from,const Text&to)const{if(from.isEmpty())return *this;auto s=value_;for(std::size_t at=0;(at=s.find(from.value_,at))!=std::string::npos;at+=to.value_.size())s.replace(at,from.value_.size(),to.value_);return Text{std::move(s)};}
    Text upToLastOccurrenceOf(const Text&needle,bool include,bool ignoreCase)const{return upToOccurrence(needle,include,ignoreCase,true);}
    Text upToFirstOccurrenceOf(const Text&needle,bool include,bool ignoreCase)const{return upToOccurrence(needle,include,ignoreCase,false);}
    Text fromLastOccurrenceOf(const Text&needle,bool include,bool ignoreCase)const{return afterOccurrence(needle,include,ignoreCase,true);}
    Text fromFirstOccurrenceOf(const Text&needle,bool include,bool ignoreCase)const{return afterOccurrence(needle,include,ignoreCase,false);}

    int getIntValue()const{return int(std::clamp(getLargeIntValue(),std::int64_t(INT32_MIN),std::int64_t(INT32_MAX)));}
    double getDoubleValue()const{
        const auto raw=numberView();double value=0;
        const auto result=detail::parseDoubleChars(raw.data(),raw.data()+raw.size(),value);
        if(result.ec!=std::errc::result_out_of_range)return result.ec==std::errc{}?value:0;
        // Distinguish overflow from underflow without a locale-dependent parser.
        // This slow path is reached only for values outside double's range.
        auto token=raw.substr(0,std::size_t(result.ptr-raw.data()));const bool negative=token.starts_with('-');
        if(negative)token.remove_prefix(1);const auto exponentAt=token.find_first_of("eE");
        const auto mantissa=token.substr(0,exponentAt);int before=0,zeros=0;bool point=false,nonzero=false;
        for(char c:mantissa){if(c=='.'){point=true;continue;}if(c<'0'||c>'9')break;if(!point)before=std::min(1000000,before+1);if(!nonzero&&c=='0')zeros=std::min(1000000,zeros+1);else nonzero=true;}
        int exponent=0;
        if(exponentAt!=std::string_view::npos){auto digits=token.substr(exponentAt+1);const bool minus=digits.starts_with('-');if(digits.starts_with('-')||digits.starts_with('+'))digits.remove_prefix(1);for(char c:digits){if(c<'0'||c>'9')break;exponent=std::min(1000000,exponent*10+(c-'0'));}if(minus)exponent=-exponent;}
        const double magnitude=!nonzero||before-zeros-1+exponent<0?0:std::numeric_limits<double>::infinity();
        return std::copysign(magnitude,negative?-1.:1.);
    }
};
}
