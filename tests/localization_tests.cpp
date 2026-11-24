// Original Cell Framework implementation; see LICENSE.
#include <cell/localization.hpp>
#include <iostream>
#include <clocale>
namespace l=cell::i18n;
namespace {void need(bool value,const char*message){if(!value)throw std::runtime_error(message);}template<class F>void rejected(F work){bool failed=false;try{work();}catch(const std::exception&){failed=true;}need(failed,"Invalid language data accepted");}}
int main(){try{
    const auto en=l::Catalog::fromJson(R"({"language":"en","messages":{"play":"Play","stop":"Stop","notes":{"one":"{count} note","other":"{count} notes"},"greeting":"Hello, {name}!"}})");
    const auto fr=l::Catalog::fromJson(R"({"language":"fr","numbers":{"decimal":",","group":" "},"messages":{"play":"Lire","greeting":"Bonjour, {name} !","notes":{"one":"{count} note","other":"{count} notes"}}})");
    const auto ca=l::Catalog::fromJson(R"({"language":"fr-CA","messages":{"play":"Lecture"}})");
    const auto ru=l::Catalog::fromJson(R"({"language":"ru","messages":{"notes":{"one":"{count} нота","few":"{count} ноты","many":"{count} нот","other":"{count} ноты"}}})");
    const auto ja=l::Catalog::fromJson(R"({"language":"ja","messages":{"play":"再生","notes":{"other":"{count} ノート"}}})");
    const auto ar=l::Catalog::fromJson(R"({"language":"ar","direction":"rtl","messages":{"play":"تشغيل"}})");
    const std::array catalogs{en,fr,ca,ru,ja,ar};
    const l::Translator french("fr_CA.UTF-8",catalogs),russian("ru-RU",catalogs),japanese("ja-JP",catalogs),arabic("ar",catalogs),english("en",catalogs);
    need(french.text("play","Play")=="Lecture"&&french.text("stop","Stop")=="Stop"&&french.text("missing","Default")=="Default","Language hierarchy/fallback failed");
    const std::array args{l::Argument{"name","Élise 声"}};
    need(french.format("greeting","Hello, {name}!",args)=="Bonjour, Élise 声 !","Named Unicode placeholders lost or kept English word order");
    auto prepared=french.prepare("greeting","Hello, {name}!");for(int i=0;i<1000;++i)need(prepared.format(args)=="Bonjour, Élise 声 !","Prepared translation changed or lost ownership");
    need(russian.plural("notes",1,"{count} note","{count} notes")=="1 нота"&&russian.plural("notes",22,"{count} note","{count} notes")=="22 ноты"&&russian.plural("notes",11,"{count} note","{count} notes")=="11 нот","Integer language plural rules failed");
    need(japanese.plural("notes",1,"{count} note","{count} notes")=="1 ノート"&&arabic.direction()==l::Direction::rightToLeft,"Catalog plural/direction metadata failed");
    need(english.text("play","Play")=="Play","A different plugin's selected language leaked into this snapshot");
    const auto broken=l::Catalog::fromJson(R"({"language":"de","messages":{"greeting":"Hallo, {wrong}!"}})");const std::array brokenCatalogs{broken,en};
    need(l::Translator("de",brokenCatalogs).format("greeting","Hello, {name}!",args)=="Hello, Élise 声!","A broken translation's placeholders were applied");
    need(l::PreparedMessage("{{{name}}}").format(args)=="{Élise 声}","Literal braces in prepared templates changed");
    l::NumberSymbols frenchNumbers;frenchNumbers.decimal=",";frenchNumbers.group=" ";
    need(l::formatDecimal(-12345.5,2,frenchNumbers)=="-12 345,50"&&l::parseDecimal("-12 345,50",frenchNumbers)==-12345.5,"Localized numbers do not round trip");
    need(!l::parseDecimal("12 34,5",frenchNumbers)&&!l::parseDecimal("1,2,3",frenchNumbers)&&!l::parseDecimal("1oops",frenchNumbers),"Invalid numeric text was partially accepted");
    l::NumberSymbols arabicNumbers;arabicNumbers.digits="٠١٢٣٤٥٦٧٨٩";arabicNumbers.decimal="٫";
    need(l::formatDecimal(12.5,1,arabicNumbers)=="١٢٫٥"&&l::parseDecimal("١٢٫٥",arabicNumbers)==12.5,"Unicode digit parsing failed");
    need(l::integerCardinal(INT64_MIN,l::CardinalRule::slavic)==l::Cardinal::many,"Minimum integer overflowed plural selection");
    rejected([]{l::normalizeLanguage("en--US");});rejected([]{l::Catalog::fromJson(R"({"language":"en","messages":{"x":{"one":"{count}"}}})");});rejected([]{l::PreparedMessage("bad {template");});
    need(std::string(std::setlocale(LC_NUMERIC,nullptr))=="C","Localization changed global numeric locale");
    std::cout<<"PASS shared localization: immutable catalogs, language hierarchy, named/prepared templates, integer plurals, RTL metadata, Unicode numerals, strict inputs and independent plugin languages\n";return 0;
}catch(const std::exception&error){std::cerr<<error.what()<<'\n';return 1;}}
