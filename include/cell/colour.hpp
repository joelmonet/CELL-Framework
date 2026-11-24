// Original Cell Framework implementation; see LICENSE.
#pragma once
#include <cell/text.hpp>
#include <algorithm>
#include <cmath>
#include <charconv>
#include <cstdint>
namespace cell {
class Colour {
    std::uint32_t argb_=0xff000000;
    static float unit(float value)noexcept{return std::isnan(value)?0.f:std::clamp(value,0.f,1.f);}
  public:
    constexpr Colour()=default;constexpr explicit Colour(std::uint32_t v):argb_(v){}
    constexpr Colour(std::uint8_t r,std::uint8_t g,std::uint8_t b):argb_(0xff000000u|(std::uint32_t(r)<<16)|(std::uint32_t(g)<<8)|b){}
    template<class T>requires requires(const T& t){t.getARGB();}Colour(const T& t):argb_(t.getARGB()){}
    cell::Text toString()const{return cell::Text::toHexString(argb_).paddedLeft('0',8);}
    static Colour fromString(const cell::Text&text){std::uint32_t bits{};const auto s=text.toStdString();const auto result=std::from_chars(s.data(),s.data()+s.size(),bits,16);return Colour{result.ec==std::errc{}&&result.ptr==s.data()+s.size()?bits:0u};}
    std::uint32_t getARGB()const{return argb_;}bool operator==(const Colour&)const=default;
    std::uint8_t getAlpha()const{return std::uint8_t(argb_>>24);}float getFloatAlpha()const{return float(getAlpha())/255.f;}
    std::uint8_t getRed()const{return std::uint8_t(argb_>>16);}std::uint8_t getGreen()const{return std::uint8_t(argb_>>8);}std::uint8_t getBlue()const{return std::uint8_t(argb_);}
    Colour withAlpha(float a)const{return Colour{(argb_&0xffffffu)|(std::uint32_t(std::lround(unit(a)*255.f))<<24)};}
    Colour withMultipliedAlpha(float a)const{return withAlpha(getFloatAlpha()*a);}
    Colour interpolatedWith(Colour c,float v)const{v=unit(v);const auto blend=[v](unsigned a,unsigned b){return std::uint32_t(std::lround(double(a)+(double(b)-a)*v));};return Colour{(blend(getAlpha(),c.getAlpha())<<24)|(blend(getRed(),c.getRed())<<16)|(blend(getGreen(),c.getGreen())<<8)|blend(getBlue(),c.getBlue())};}
    Colour overlaidWith(Colour c)const{const float a=c.getFloatAlpha(),b=getFloatAlpha()*(1-a),total=a+b;if(total<=0)return Colour{0u};const auto channel=[=](unsigned x,unsigned y){return std::uint32_t(std::lround((double(y)*a+double(x)*b)/total));};return Colour{(std::uint32_t(std::lround(total*255))<<24)|(channel(getRed(),c.getRed())<<16)|(channel(getGreen(),c.getGreen())<<8)|channel(getBlue(),c.getBlue())};}
    Colour brighter(float amount=.4f)const{return interpolatedWith(Colour{0xffffffffu},unit(amount)).withAlpha(getFloatAlpha());}
    Colour darker(float amount=.4f)const{return interpolatedWith(Colour{0xff000000u},unit(amount)).withAlpha(getFloatAlpha());}
    float getBrightness()const{return float(std::max({getRed(),getGreen(),getBlue()}))/255.f;}
    static Colour fromFloatRGBA(float r,float g,float b,float a){const auto byte=[](float f){return std::uint32_t(std::lround(unit(f)*255));};return Colour{(byte(a)<<24)|(byte(r)<<16)|(byte(g)<<8)|byte(b)};}
};
}
