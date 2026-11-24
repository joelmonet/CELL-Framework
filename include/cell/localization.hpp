// Original Cell Framework implementation; see LICENSE.
#pragma once
#include <cell/json.hpp>
#include <memory>
#include <span>
#include <optional>

namespace cell::i18n {
enum class Direction {leftToRight,rightToLeft};
enum class Cardinal {zero,one,two,few,many,other};
enum class CardinalRule {languageDefault,oneOther,otherOnly,french,slavic,polish,czech,arabic,hebrew};
struct Argument {std::string_view name;Text value;};
struct Message {Text text;std::map<Cardinal,Text>forms;};
using Messages=std::map<std::string,Message,std::less<>>;
struct NumberSymbols {Text decimal=".",group,minus="-",digits="0123456789";unsigned grouping=3;};

Text normalizeLanguage(const Text&);
Cardinal integerCardinal(std::int64_t,CardinalRule)noexcept;
Text formatInteger(std::int64_t,const NumberSymbols& = {});
Text formatDecimal(double,int places,const NumberSymbols& = {});
std::optional<double>parseDecimal(const Text&,const NumberSymbols& = {});

// Immutable per-language resources, loaded/validated outside painting/audio.
// There is no process-wide locale change or dependence on an OS locale pack.
class Catalog {
    Text language_;Direction direction_;CardinalRule rule_;NumberSymbols numbers_;Messages messages_;
    Catalog(Text,Messages,Direction,CardinalRule,NumberSymbols);
public:
    static std::shared_ptr<const Catalog> create(Text language,Messages,
        Direction=Direction::leftToRight,CardinalRule=CardinalRule::languageDefault,NumberSymbols={});
    static std::shared_ptr<const Catalog> fromJson(std::string_view,const json::Limits& = {});
    const Text&language()const noexcept{return language_;}
    Direction direction()const noexcept{return direction_;}
    CardinalRule cardinalRule()const noexcept{return rule_;}
    const NumberSymbols&numbers()const noexcept{return numbers_;}
    const Message*find(std::string_view)const noexcept;
};

// Compile once when binding a control; repeated formatting only assembles its
// output and argument values. There is no template parsing or global lookup.
class PreparedMessage {
    struct Impl;std::shared_ptr<const Impl>impl_;
public:
    explicit PreparedMessage(Text pattern);
    Text format(std::span<const Argument>)const;
};

// A ready UI snapshot. Select outside rendering and retain it for a frame or
// editor lifetime. Replacing one plugin's snapshot cannot change another's.
class Translator {
    Text requested_;std::vector<std::shared_ptr<const Catalog>>chain_;
    struct Found {const Catalog*catalog{};const Message*message{};};
    Found find(std::string_view)const noexcept;
public:
    Translator(Text requested,std::span<const std::shared_ptr<const Catalog>>available,Text fallback="en");
    const Text&language()const noexcept{return requested_;}
    Direction direction()const noexcept;
    const NumberSymbols&numbers()const noexcept;
    // Borrowed until this snapshot/catalog or the caller's fallback is released.
    std::string_view view(std::string_view key,std::string_view fallback)const noexcept;
    Text text(std::string_view key,Text fallback)const;
    Text format(std::string_view key,Text fallback,std::span<const Argument>)const;
    PreparedMessage prepare(std::string_view key,Text fallback)const;
    Text plural(std::string_view key,std::int64_t count,Text one,Text other,std::span<const Argument> = {})const;
};
}
