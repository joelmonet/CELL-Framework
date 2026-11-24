// SPDX-License-Identifier: Apache-2.0
#pragma once
#include <cell/parameters.hpp>
#include <limits>
#include <optional>

namespace cell::state {
// Legacy float parameter trees can contain a few ULPs of error after a
// normalized/plain conversion. Admit only that representation noise, never a
// different control setting. Continuous sub-unit values retain their precision.
inline std::optional<double> compatibleParameterValue(const Parameter& field,double value)noexcept{
    if(!std::isfinite(value))return {};
    const auto tolerance=[](double a,double b){return 2.*std::numeric_limits<float>::epsilon()*std::max({1.,std::abs(a),std::abs(b)});};
    if(value<field.minimum){if(field.minimum-value>tolerance(value,field.minimum))return {};value=field.minimum;}
    if(value>field.maximum){if(value-field.maximum>tolerance(value,field.maximum))return {};value=field.maximum;}
    if(field.step>=1){
        const double nearest=field.clamp(value);
        if(std::abs(nearest-value)>std::min(field.step*.01,tolerance(nearest,value)))return {};
        value=nearest;
    }
    return value;
}
}
