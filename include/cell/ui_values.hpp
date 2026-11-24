#pragma once
#include <cell/parameters.hpp>
#include <cell/text.hpp>
#include <charconv>
#include <cctype>
#include <string>
#include <string_view>

namespace cell::ui {
enum class ValueUnit {number,percent,decibels,hertz,milliseconds,seconds,ratio,semitones,cents,q,bpm};
struct ValueFormat {
 ValueUnit unit=ValueUnit::number;unsigned decimals=1;bool signedPositive{};
 bool compactFrequency=true,silenceAtMinimum{},infiniteAtMaximum{};
};
inline std::string_view trimValueText(std::string_view text)noexcept{
 while(!text.empty()&&std::isspace(static_cast<unsigned char>(text.front())))text.remove_prefix(1);
 while(!text.empty()&&std::isspace(static_cast<unsigned char>(text.back())))text.remove_suffix(1);
 return text;
}
inline bool sameValueUnit(std::string_view a,std::string_view b)noexcept{
 if(a.size()!=b.size())return false;
 for(std::size_t i=0;i<a.size();++i)if(std::tolower(static_cast<unsigned char>(a[i]))!=std::tolower(static_cast<unsigned char>(b[i])))return false;
 return true;
}
inline std::string_view valueUnitSuffix(ValueUnit unit)noexcept{
 switch(unit){case ValueUnit::percent:return "%";case ValueUnit::decibels:return "dB";case ValueUnit::hertz:return "Hz";case ValueUnit::milliseconds:return "ms";case ValueUnit::seconds:return "s";case ValueUnit::ratio:return ":1";case ValueUnit::semitones:return "st";case ValueUnit::cents:return "ct";case ValueUnit::q:return "Q";case ValueUnit::bpm:return "BPM";default:return {};}
}
inline std::string formatValue(const Parameter& field,double plain,ValueFormat format={}){
 if(!std::isfinite(plain))return "—";
 auto suffix=valueUnitSuffix(format.unit);
 if(format.unit==ValueUnit::decibels&&format.silenceAtMinimum&&plain<=field.minimum)return std::string("−∞")+(suffix.empty()?"":" ")+std::string(suffix);
 if(format.unit==ValueUnit::ratio&&format.infiniteAtMaximum&&plain>=field.maximum)return "∞:1";
 if(format.unit==ValueUnit::hertz&&format.compactFrequency&&std::abs(plain)>=1000){plain/=1000;suffix="kHz";}
 if(std::abs(plain)<.5*std::pow(10.,-int(std::min(format.decimals,6u))))plain=0;
 char text[96];const auto converted=std::to_chars(text,text+sizeof(text),plain,std::chars_format::fixed,int(std::min(format.decimals,6u)));
 if(converted.ec!=std::errc{})return "—";
 std::string result;if(format.signedPositive&&plain>0)result+='+';result.append(text,converted.ptr);
 if(!suffix.empty()){if(format.unit!=ValueUnit::ratio)result+=' ';result+=suffix;}
 return result;
}
// Parsing never silently clips an out-of-range value or accepts a partial token.
// Semantic infinity/silence is enabled only when the processor defines it.
inline bool parseValue(const Parameter& field,std::string_view text,ValueFormat format,double& plain)noexcept{
 text=trimValueText(text);if(text.empty())return false;
 if(format.unit==ValueUnit::decibels&&format.silenceAtMinimum){
  auto value=text;const auto suffix=valueUnitSuffix(format.unit);
  if(value.size()>=suffix.size()&&sameValueUnit(value.substr(value.size()-suffix.size()),suffix))value=trimValueText(value.substr(0,value.size()-suffix.size()));
  if(sameValueUnit(value,"-inf")||value=="−∞"||value=="-∞"){plain=field.minimum;return true;}
 }
 if(format.unit==ValueUnit::ratio&&format.infiniteAtMaximum&&(sameValueUnit(text,"inf")||sameValueUnit(text,"inf:1")||text=="∞"||text=="∞:1")){plain=field.maximum;return true;}
 if(text.front()=='+'){text.remove_prefix(1);if(text.empty()||text.front()=='+'||text.front()=='-')return false;}
 double value{};const auto converted=cell::detail::parseDoubleChars(text.data(),text.data()+text.size(),value);
 if(converted.ec!=std::errc{}||!std::isfinite(value))return false;
 const auto suffix=trimValueText(text.substr(std::size_t(converted.ptr-text.data())));
 double multiplier=1;
 if(!suffix.empty()&&!sameValueUnit(suffix,valueUnitSuffix(format.unit))){
  if(format.unit==ValueUnit::hertz&&sameValueUnit(suffix,"kHz"))multiplier=1000;
  else if(format.unit==ValueUnit::milliseconds&&sameValueUnit(suffix,"s"))multiplier=1000;
  else if(format.unit==ValueUnit::seconds&&sameValueUnit(suffix,"ms"))multiplier=.001;
  else return false;
 }
 value*=multiplier;if(!std::isfinite(value)||value<field.minimum||value>field.maximum)return false;
 plain=field.clamp(value);return true;
}
}
