// SPDX-License-Identifier: Apache-2.0
#pragma once
#include <cell/parameters.hpp>
#include <optional>
namespace cell::ui {
// Pointer geometry only: never change the host's normalization or saved value.
inline std::optional<double> knobUnity(const Parameter& p)noexcept{
 if(p.minimum<0&&p.maximum>0)return 0.;
 if(p.minimum<=0&&p.maximum>=0&&(p.key.ends_with("Output")||p.key.ends_with("Trim")||p.key=="msMid"||p.key=="msSide"))return 0.;
 if(p.minimum<=1&&p.maximum>=1&&(p.unit=="x"||p.unit=="X"||p.key=="duration"||p.key.ends_with("Ratio")||((p.key.ends_with("Gain")||p.key.ends_with("Level"))&&p.unit!="%"&&p.maximum<=4)))return 1.;
 return {};
}
inline double knobPosition(const Parameter& p,double normalized,double normalizedUnity)noexcept{
 if(!knobUnity(p))return std::clamp(normalized,0.,1.);
 if(normalizedUnity<=0)return .5+.5*std::clamp(normalized,0.,1.);
 if(normalizedUnity>=1)return .5*std::clamp(normalized,0.,1.);
 return std::clamp(normalized<=normalizedUnity?.5*normalized/normalizedUnity:.5+.5*(normalized-normalizedUnity)/(1-normalizedUnity),0.,1.);
}
inline double knobPosition(const Parameter& p,double value)noexcept{
 const auto unity=knobUnity(p);return unity?knobPosition(p,p.toNormalized(value),p.toNormalized(*unity)):p.toNormalized(value);
}
}
