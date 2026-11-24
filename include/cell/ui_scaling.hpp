#pragma once
#include <cell/ui.hpp>
#include <cmath>

namespace cell::ui {
inline constexpr double maximumNativeExtent=8192;
inline bool validContentScale(double factor)noexcept {
    return std::isfinite(factor)&&factor>=.25&&factor<=8;
}
inline bool validNativeSize(Point size)noexcept {
    return std::isfinite(size.x)&&std::isfinite(size.y)&&size.x>=1&&size.y>=1&&
        size.x<=maximumNativeExtent&&size.y<=maximumNativeExtent;
}
inline Point logicalPoint(Point native,double factor)noexcept{return {native.x/factor,native.y/factor};}
inline Point nativePoint(Point logical,double factor)noexcept{return {logical.x*factor,logical.y*factor};}

// Native host coordinates, logical editor coordinates and raster backing
// pixels are distinct. This model owns only the first two. Cocoa's backing
// scale is applied by its renderer, independently of the host's content zoom.
class ViewGeometry {
public:
    explicit ViewGeometry(Point logical={1000,620}):logical_(logical) {
        if(!validNativeSize(logical))throw std::invalid_argument("Invalid logical editor size");
    }
    Point logicalSize()const noexcept{return logical_;}
    double scale()const noexcept{return scale_;}
    Point nativeSize()const noexcept{return nativeSize(logical_,scale_);}
    static Point nativeSize(Point logical,double factor)noexcept {
        const auto size=nativePoint(logical,factor);return {std::round(size.x),std::round(size.y)};
    }
    bool setScale(double factor)noexcept {
        if(!validContentScale(factor)||!validNativeSize(nativeSize(logical_,factor)))return false;
        scale_=factor;return true;
    }
    bool setLogicalSize(Point size)noexcept {
        if(!validNativeSize(size)||!validNativeSize(nativeSize(size,scale_)))return false;
        logical_=size;return true;
    }
    bool resizeNative(Point size)noexcept {
        if(!validNativeSize(size))return false;
        const auto expected=nativeSize();
        // Host acknowledgement of a rounded DPI request must not feed the
        // rounded pixels back into the saved logical dimensions. Repeated
        // monitor changes would otherwise accumulate fractional-size drift.
        if(size.x!=expected.x||size.y!=expected.y){const auto logical=logicalPoint(size,scale_);if(!validNativeSize(logical))return false;logical_=logical;}
        return true;
    }
private:
    Point logical_;
    double scale_=1;
};
}
