// Original Cell Framework implementation; see LICENSE.
#pragma once
#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <limits>
#include <stdexcept>
#include <utility>
namespace cell {
template<class T>class ScopedValue {
 T&target_;T previous_;
 public:ScopedValue(T&target,T replacement):target_(target),previous_(target){target_=std::move(replacement);}~ScopedValue(){target_=std::move(previous_);}ScopedValue(const ScopedValue&)=delete;ScopedValue&operator=(const ScopedValue&)=delete;
};
struct Clock {
 static std::int64_t getHighResolutionTicks()noexcept{return std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now().time_since_epoch()).count();}
 static constexpr std::int64_t getHighResolutionTicksPerSecond()noexcept{return 1000000000;}
 static double getMillisecondCounterHiRes()noexcept{return double(getHighResolutionTicks())*.000001;}
 static std::uint64_t getMillisecondCounter()noexcept{return std::uint64_t(getHighResolutionTicks()/1000000);}
};
namespace math {
template<class T>struct Constants {static constexpr T pi=T(3.141592653589793238462643383279502884L),twoPi=T(2)*pi,halfPi=pi/T(2);};
template<class T>constexpr T limit(T lower,T upper,T value)noexcept{return std::min(upper,std::max(lower,value));}
inline int roundToInt(double value)noexcept{return int(std::nearbyint(value));}
}
}
