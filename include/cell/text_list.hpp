// Original Cell Framework implementation; see LICENSE.
#pragma once
#include <cell/text.hpp>
#include <vector>
#include <ranges>
#include <unordered_set>

namespace cell {
// Owning, dynamically sized control-thread text. A list does not retain a
// foreign container when constructed at a temporary framework boundary.
class TextList {
    std::vector<Text> values_;
public:
    TextList()=default;
    TextList(std::initializer_list<Text> values):values_(values){}
    template<std::ranges::input_range Range>
      requires(!std::is_same_v<std::remove_cvref_t<Range>,TextList>&&
               !std::is_convertible_v<Range,std::string_view>&&
               !requires(const Range&r){r.toRawUTF8();})
    TextList(const Range& values){for(const auto&value:values)values_.emplace_back(value);}
    auto begin()noexcept{return values_.begin();}auto end()noexcept{return values_.end();}
    auto begin()const noexcept{return values_.begin();}auto end()const noexcept{return values_.end();}
    std::size_t size()const noexcept{return values_.size();}bool isEmpty()const noexcept{return values_.empty();}
    const Text&operator[](std::size_t at)const{return values_.at(at);}Text&operator[](std::size_t at){return values_.at(at);}
    const Text&getReference(std::size_t at)const{return values_.at(at);}Text&getReference(std::size_t at){return values_.at(at);}
    void clear()noexcept{values_.clear();}void ensureStorageAllocated(std::size_t count){values_.reserve(count);}
    void add(Text value){values_.push_back(std::move(value));}
    template<std::ranges::input_range Range>void addArray(const Range&values){for(const auto&value:values)add(Text(value));}
    bool contains(const Text&value,bool ignoreCase=false)const{return std::any_of(begin(),end(),[&](const auto&item){return ignoreCase?item.equalsIgnoreCase(value):item==value;});}
    bool addIfNotAlreadyThere(Text value,bool ignoreCase=false){if(contains(value,ignoreCase))return false;add(std::move(value));return true;}
    std::ptrdiff_t indexOf(const Text&value,bool ignoreCase=false)const{for(std::size_t at=0;at<size();++at)if(ignoreCase?values_[at].equalsIgnoreCase(value):values_[at]==value)return std::ptrdiff_t(at);return -1;}
    void remove(std::size_t at){if(at<size())values_.erase(values_.begin()+std::ptrdiff_t(at));}
    void removeString(const Text&value,bool ignoreCase=false){std::erase_if(values_,[&](const auto&item){return ignoreCase?item.equalsIgnoreCase(value):item==value;});}
    void trim(){for(auto&value:values_)value=value.trim();}
    void removeEmptyStrings(bool trimBeforeCheck=false){std::erase_if(values_,[&](const auto&value){return trimBeforeCheck?value.trim().isEmpty():value.isEmpty();});}
    void removeDuplicates(bool ignoreCase=false){std::unordered_set<std::string>seen;std::erase_if(values_,[&](const auto&value){return !seen.insert((ignoreCase?value.caseFolded():value).toStdString()).second;});}
    void sort(bool ignoreCase=false){std::sort(begin(),end(),[&](const auto&a,const auto&b){return ignoreCase?a.compareIgnoreCase(b)<0:a<b;});}
    void sortNatural(bool ignoreCase=true){std::stable_sort(begin(),end(),[&](const auto&a,const auto&b){return a.compareNatural(b,ignoreCase)<0;});}
    bool operator==(const TextList&)const=default;
    Text joinIntoString(const Text&separator,std::size_t first=0,std::ptrdiff_t count=-1)const{
        first=std::min(first,size());const auto end=first+(count<0?size()-first:std::min(size()-first,std::size_t(count)));
        Text result;for(auto at=first;at<end;++at){if(at!=first)result+=separator;result+=values_[at];}return result;
    }
    std::size_t addLines(const Text&text){
        const auto original=size();const auto input=text.utf8();std::size_t begin=0;
        for(std::size_t at=0;at<input.size();++at)if(input[at]=='\r'||input[at]=='\n'){
            add(Text(input.substr(begin,at-begin)));if(input[at]=='\r'&&at+1<input.size()&&input[at+1]=='\n')++at;begin=at+1;
        }
        add(Text(input.substr(begin)));return size()-original;
    }
    static TextList fromLines(const Text&text){TextList result;result.addLines(text);return result;}
    std::size_t addTokens(const Text&text,const Text&separators,const Text&quotes){
        const auto original=size();Text token;char32_t quote{};bool occupied=false;
        for(auto code:text){
            if(quote){if(code==quote)quote=0;else token+=code;occupied=true;}
            else if(quotes.indexOfChar(code)>=0){quote=code;occupied=true;}
            else if(separators.indexOfChar(code)>=0){if(occupied){add(std::move(token));token={};occupied=false;}}
            else{token+=code;occupied=true;}
        }
        if(occupied)add(std::move(token));return size()-original;
    }
    std::size_t addTokens(const Text&text,bool preserveQuotedStrings){return addTokens(text," \t\r\n",preserveQuotedStrings?Text("\"'"):Text{});}
    static TextList fromTokens(const Text&text,const Text&separators,const Text&quotes){TextList result;result.addTokens(text,separators,quotes);return result;}
};
}
