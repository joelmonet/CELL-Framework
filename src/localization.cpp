// Original Cell Framework implementation; see LICENSE.
#include <cell/localization.hpp>
#include <set>

namespace cell::i18n {
namespace {
std::set<std::string>parameters(const Text&text){
    std::set<std::string>result;const auto raw=text.utf8();
    for(std::size_t at=0;at<raw.size();++at){
        if(raw[at]=='{'&&at+1<raw.size()&&raw[at+1]=='{'){++at;continue;}
        if(raw[at]=='}'&&at+1<raw.size()&&raw[at+1]=='}'){++at;continue;}
        if(raw[at]=='}')throw std::invalid_argument("Unescaped localization closing brace");
        if(raw[at]!='{')continue;
        const auto end=raw.find('}',at+1);if(end==std::string_view::npos||end==at+1)throw std::invalid_argument("Invalid localization placeholder");
        const auto name=raw.substr(at+1,end-at-1);for(char code:name)if(!((code>='a'&&code<='z')||(code>='A'&&code<='Z')||(code>='0'&&code<='9')||code=='_'||code=='.'||code=='-'))throw std::invalid_argument("Invalid localization placeholder name");
        result.emplace(name);at=end;
    }return result;
}
Text render(const Text&text,std::span<const Argument>arguments){
    const auto raw=text.utf8();std::string out;out.reserve(raw.size());
    for(std::size_t at=0;at<raw.size();){
        if(at+1<raw.size()&&((raw[at]=='{'&&raw[at+1]=='{')||(raw[at]=='}'&&raw[at+1]=='}'))){out+=raw[at];at+=2;continue;}
        if(raw[at]!='{'){out+=raw[at++];continue;}
        const auto end=raw.find('}',at+1);if(end==std::string_view::npos){out.append(raw.substr(at));break;}
        const auto name=raw.substr(at+1,end-at-1);const auto found=std::find_if(arguments.begin(),arguments.end(),[&](const auto&argument){return argument.name==name;});
        if(found!=arguments.end())out+=found->value.toStdString();else out.append(raw.substr(at,end-at+1));at=end+1;
    }return Text(std::move(out));
}
Text numberText(Text raw,const NumberSymbols&symbols){
    const auto input=raw.utf8();const auto split=input.find('.');auto integer=input.substr(0,split);
    const auto fraction=split==std::string_view::npos?std::string_view{}:input.substr(split+1);
    std::array<Text,10>digits;std::size_t next{};for(auto code:symbols.digits)digits[next++]=Text::charToString(code);
    Text result;result.preallocateBytes(input.size());
    if(integer.starts_with('-')){result+=symbols.minus;integer.remove_prefix(1);}
    for(std::size_t at=0;at<integer.size();++at){if(at&&symbols.grouping&&symbols.group.isNotEmpty()&&(integer.size()-at)%symbols.grouping==0)result+=symbols.group;const auto code=integer[at];if(code>='0'&&code<='9')result+=digits[std::size_t(code-'0')];else result+=char32_t(code);}
    if(split!=std::string_view::npos){result+=symbols.decimal;for(auto code:fraction){if(code>='0'&&code<='9')result+=digits[std::size_t(code-'0')];else result+=char32_t(code);}}
    return result;
}
void validateNumbers(const NumberSymbols&symbols){
    std::set<char32_t>digits;for(auto code:symbols.digits)digits.insert(code);
    if(symbols.digits.length()!=10||digits.size()!=10||symbols.decimal.isEmpty()||symbols.minus.isEmpty()||symbols.decimal==symbols.group)throw std::invalid_argument("Invalid localization number symbols");
}
CardinalRule ruleFor(const Text&language){const auto base=language.upToFirstOccurrenceOf("-",false,false);if(base=="ja"||base=="zh"||base=="ko"||base=="th"||base=="vi")return CardinalRule::otherOnly;if(base=="fr")return CardinalRule::french;if(base=="ru"||base=="uk"||base=="be")return CardinalRule::slavic;if(base=="pl")return CardinalRule::polish;if(base=="cs"||base=="sk")return CardinalRule::czech;if(base=="ar")return CardinalRule::arabic;if(base=="he")return CardinalRule::hebrew;return CardinalRule::oneOther;}
Cardinal category(std::string_view name){if(name=="zero")return Cardinal::zero;if(name=="one")return Cardinal::one;if(name=="two")return Cardinal::two;if(name=="few")return Cardinal::few;if(name=="many")return Cardinal::many;if(name=="other")return Cardinal::other;throw std::invalid_argument("Unknown localization plural category");}
}
Text normalizeLanguage(const Text&source){
    const auto value=source.trim().upToFirstOccurrenceOf(".",false,false).upToFirstOccurrenceOf("@",false,false).replace("_","-").toLowerCase();
    if(value=="c"||value=="posix")return "en";
    if(value.isEmpty()||value[0]<'a'||value[0]>'z')throw std::invalid_argument("Invalid language identifier");
    std::size_t segment{};for(auto code:value){if(code=='-'){if(!segment||segment>8)throw std::invalid_argument("Invalid language subtag");segment=0;}else{if(!((code>='a'&&code<='z')||(code>='0'&&code<='9')))throw std::invalid_argument("Invalid language identifier");++segment;}}
    if(!segment||segment>8)throw std::invalid_argument("Invalid language subtag");return value;
}
Cardinal integerCardinal(std::int64_t value,CardinalRule rule)noexcept{
    const auto n=value<0?std::uint64_t(-(value+1))+1:std::uint64_t(value),last=n%10,hundred=n%100;
    switch(rule){
    case CardinalRule::otherOnly:return Cardinal::other;
    case CardinalRule::french:return n==0||n==1?Cardinal::one:Cardinal::other;
    case CardinalRule::slavic:return last==1&&hundred!=11?Cardinal::one:last>=2&&last<=4&&(hundred<12||hundred>14)?Cardinal::few:Cardinal::many;
    case CardinalRule::polish:return n==1?Cardinal::one:last>=2&&last<=4&&(hundred<12||hundred>14)?Cardinal::few:Cardinal::many;
    case CardinalRule::czech:return n==1?Cardinal::one:n>=2&&n<=4?Cardinal::few:Cardinal::other;
    case CardinalRule::arabic:return n==0?Cardinal::zero:n==1?Cardinal::one:n==2?Cardinal::two:hundred>=3&&hundred<=10?Cardinal::few:hundred>=11&&hundred<=99?Cardinal::many:Cardinal::other;
    case CardinalRule::hebrew:return n==1?Cardinal::one:n==2?Cardinal::two:Cardinal::other;
    case CardinalRule::languageDefault:case CardinalRule::oneOther:return n==1?Cardinal::one:Cardinal::other;
    }return Cardinal::other;
}
Text formatInteger(std::int64_t value,const NumberSymbols&symbols){validateNumbers(symbols);return numberText(Text(value),symbols);}
Text formatDecimal(double value,int places,const NumberSymbols&symbols){validateNumbers(symbols);if(!std::isfinite(value))throw std::invalid_argument("Cannot localize a non-finite number");return numberText(Text(value,places),symbols);}
std::optional<double>parseDecimal(const Text&value,const NumberSymbols&symbols){
    validateNumbers(symbols);const auto trimmed=value.trim();auto input=trimmed.utf8();std::string raw;raw.reserve(input.size());
    if(input.starts_with(symbols.minus.utf8())){raw+='-';input.remove_prefix(symbols.minus.getNumBytesAsUTF8());}
    else if(input.starts_with('-')||input.starts_with('+')){if(input.front()=='-')raw+='-';input.remove_prefix(1);}
    const auto decimal=input.find(symbols.decimal.utf8());auto integer=input.substr(0,decimal);
    const auto fraction=decimal==std::string_view::npos?std::string_view{}:input.substr(decimal+symbols.decimal.getNumBytesAsUTF8());
    const auto digit=[&](char32_t code)->int{int at=0;for(auto symbol:symbols.digits){if(symbol==code)return at;++at;}return code>='0'&&code<='9'?int(code-'0'):-1;};
    if(symbols.grouping&&symbols.group.isNotEmpty()&&integer.find(symbols.group.utf8())!=std::string_view::npos){
        std::size_t at{},part{};while(at<=integer.size()){const auto end=integer.find(symbols.group.utf8(),at);const auto segment=Text(integer.substr(at,end==std::string_view::npos?end:end-at));if(segment.isEmpty()||(!part&&unsigned(segment.length())>symbols.grouping)||(part&&unsigned(segment.length())!=symbols.grouping))return {};for(auto code:segment){const int number=digit(code);if(number<0)return {};raw+=char('0'+number);}if(end==std::string_view::npos)break;at=end+symbols.group.getNumBytesAsUTF8();++part;}
    }else for(auto code:Text(integer)){const int number=digit(code);if(number<0)return {};raw+=char('0'+number);}
    if(decimal!=std::string_view::npos){raw+='.';if(fraction.empty())return {};for(auto code:Text(fraction)){const int number=digit(code);if(number<0)return {};raw+=char('0'+number);}}
    double result{};const auto parsed=cell::detail::parseDoubleChars(raw.data(),raw.data()+raw.size(),result);
    if(parsed.ec!=std::errc{}||parsed.ptr!=raw.data()+raw.size()||!std::isfinite(result))return {};return result;
}
struct PreparedMessage::Impl {
    struct Piece {std::size_t first,count;bool argument;};Text text;std::vector<Piece>pieces;
    explicit Impl(Text value):text(std::move(value)){
        parameters(text);const auto raw=text.utf8();std::size_t literal{};
        for(std::size_t at=0;at<raw.size();){
            if(at+1<raw.size()&&((raw[at]=='{'&&raw[at+1]=='{')||(raw[at]=='}'&&raw[at+1]=='}'))){if(at>literal)pieces.push_back({literal,at-literal,false});pieces.push_back({at,1,false});at+=2;literal=at;}
            else if(raw[at]=='{'){if(at>literal)pieces.push_back({literal,at-literal,false});const auto end=raw.find('}',at+1);pieces.push_back({at+1,end-at-1,true});at=end+1;literal=at;}
            else ++at;
        }if(literal<raw.size())pieces.push_back({literal,raw.size()-literal,false});
    }
};
PreparedMessage::PreparedMessage(Text pattern):impl_(std::make_shared<const Impl>(std::move(pattern))){}
Text PreparedMessage::format(std::span<const Argument>arguments)const{
    std::string result;result.reserve(impl_->text.getNumBytesAsUTF8());const auto raw=impl_->text.utf8();
    for(const auto&piece:impl_->pieces){const auto text=raw.substr(piece.first,piece.count);if(!piece.argument){result.append(text);continue;}const auto found=std::find_if(arguments.begin(),arguments.end(),[&](const auto&argument){return argument.name==text;});if(found!=arguments.end())result+=found->value.toStdString();else{result+='{';result.append(text);result+='}';}}
    return Text(std::move(result));
}
Catalog::Catalog(Text language,Messages messages,Direction direction,CardinalRule rule,NumberSymbols numbers):language_(std::move(language)),direction_(direction),rule_(rule),numbers_(std::move(numbers)),messages_(std::move(messages)){}
std::shared_ptr<const Catalog>Catalog::create(Text language,Messages messages,Direction direction,CardinalRule rule,NumberSymbols numbers){
    language=normalizeLanguage(language);if(rule==CardinalRule::languageDefault)rule=ruleFor(language);validateNumbers(numbers);
    for(const auto&[key,message]:messages){if(key.empty())throw std::invalid_argument("Empty localization key");if(message.forms.empty())parameters(message.text);else{if(!message.forms.contains(Cardinal::other))throw std::invalid_argument("Plural message needs an other form");for(const auto&[form,text]:message.forms){(void)form;parameters(text);}}}
    return std::shared_ptr<const Catalog>(new Catalog(std::move(language),std::move(messages),direction,rule,std::move(numbers)));
}
std::shared_ptr<const Catalog>Catalog::fromJson(std::string_view input,const json::Limits&limits){
    const auto root=json::read(input,limits);if(!root.isObject()||!root["language"].isString()||!root["messages"].isObject())throw std::invalid_argument("Invalid localization catalog");
    const auto language=normalizeLanguage(Text(root["language"].text()));const auto direction=root["direction"].text();if(!direction.empty()&&direction!="ltr"&&direction!="rtl")throw std::invalid_argument("Invalid catalog direction");
    Messages messages;for(const auto&[key,value]:*root["messages"].object()){
        Message message;if(value.isString())message.text=Text(value.text());else if(value.isObject()){for(const auto&[name,text]:*value.object()){if(!text.isString())throw std::invalid_argument("Plural forms must be text");message.forms.emplace(category(name),Text(text.text()));}}else throw std::invalid_argument("Catalog messages must be text or plural forms");messages.emplace(key,std::move(message));
    }
    NumberSymbols numbers;if(const auto*values=root["numbers"].object()){(void)values;const auto&n=root["numbers"];for(auto [key,target]:{std::pair{"decimal",&numbers.decimal},{"group",&numbers.group},{"minus",&numbers.minus},{"digits",&numbers.digits}})if(n[key].isString())*target=Text(n[key].text());if(n["grouping"].isNumber()){const auto value=*n["grouping"].number();if(!std::isfinite(value)||value<0||value>unsigned(-1)||std::floor(value)!=value)throw std::invalid_argument("Invalid number grouping");numbers.grouping=unsigned(value);}}
    return create(language,std::move(messages),direction=="rtl"?Direction::rightToLeft:Direction::leftToRight,ruleFor(language),std::move(numbers));
}
const Message*Catalog::find(std::string_view key)const noexcept{const auto found=messages_.find(key);return found==messages_.end()?nullptr:&found->second;}
Translator::Translator(Text requested,std::span<const std::shared_ptr<const Catalog>>available,Text fallback):requested_(normalizeLanguage(requested)){
    auto append=[&](Text language){for(;;){for(const auto&catalog:available)if(catalog&&catalog->language()==language&&std::find(chain_.begin(),chain_.end(),catalog)==chain_.end())chain_.push_back(catalog);const auto split=language.lastIndexOfChar('-');if(split<0)break;language=language.substring(0,split);}};
    append(requested_);append(normalizeLanguage(fallback));
}
Translator::Found Translator::find(std::string_view key)const noexcept{for(const auto&catalog:chain_)if(const auto*message=catalog->find(key))return {catalog.get(),message};return {};}
Direction Translator::direction()const noexcept{return chain_.empty()?Direction::leftToRight:chain_.front()->direction();}
const NumberSymbols&Translator::numbers()const noexcept{static const NumberSymbols defaults;return chain_.empty()?defaults:chain_.front()->numbers();}
std::string_view Translator::view(std::string_view key,std::string_view fallback)const noexcept{const auto found=find(key);return found.message&&found.message->forms.empty()?found.message->text.utf8():fallback;}
Text Translator::text(std::string_view key,Text fallback)const{return Text(view(key,fallback.utf8()));}
PreparedMessage Translator::prepare(std::string_view key,Text fallback)const{const auto found=find(key);return PreparedMessage(found.message&&found.message->forms.empty()&&parameters(found.message->text)==parameters(fallback)?found.message->text:std::move(fallback));}
Text Translator::format(std::string_view key,Text fallback,std::span<const Argument>arguments)const{return prepare(key,std::move(fallback)).format(arguments);}
Text Translator::plural(std::string_view key,std::int64_t count,Text one,Text other,std::span<const Argument>arguments)const{
    const auto fallback=integerCardinal(count,CardinalRule::oneOther)==Cardinal::one?one:other;const auto found=find(key);Text selected=fallback;
    if(found.message&&!found.message->forms.empty()){const auto form=integerCardinal(count,found.catalog->cardinalRule());auto choice=found.message->forms.find(form);if(choice==found.message->forms.end())choice=found.message->forms.find(Cardinal::other);if(choice!=found.message->forms.end()&&parameters(choice->second)==parameters(fallback))selected=choice->second;}
    std::vector<Argument>values(arguments.begin(),arguments.end());std::erase_if(values,[](const auto&argument){return argument.name=="count";});values.push_back({"count",formatInteger(count,numbers())});return render(selected,values);
}
}
