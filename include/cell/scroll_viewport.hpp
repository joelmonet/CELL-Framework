// SPDX-License-Identifier: Apache-2.0
#pragma once
#include <algorithm>
#include <cmath>
namespace cell::ui {
// Logical coordinates: use the same measured bounds as painting/hit testing.
inline double clampViewportScroll(double offset,double extent,double content)noexcept{
 if(!std::isfinite(offset)||!std::isfinite(extent)||!std::isfinite(content))return 0;
 return std::clamp(offset,0.,std::max(0.,content-std::max(0.,extent)));
}
inline double revealViewportScroll(double offset,double extent,double content,double begin,double end)noexcept{
 offset=clampViewportScroll(offset,extent,content);
 if(!std::isfinite(begin)||!std::isfinite(end)||end<begin||extent<=0)return offset;
 if(begin<offset)offset=begin;
 else if(end>offset+extent)offset=end-begin>extent?begin:end-extent;
 return clampViewportScroll(offset,extent,content);
}
// A captured resize follows the trailing edge and reverses its own scrolling
// when the module shrinks again. All values use logical content coordinates.
inline double resizeViewportScroll(double originalOffset,double extent,double content,double trailingEdge)noexcept{
 if(!std::isfinite(originalOffset)||!std::isfinite(extent)||!std::isfinite(content)||!std::isfinite(trailingEdge)||extent<=0)return 0;
 return clampViewportScroll(std::max(originalOffset,trailingEdge-extent),extent,content);
}
}
