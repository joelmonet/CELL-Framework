// Original Cell Framework implementation; see LICENSE.
#pragma once
#include <charconv>
#include <cell/colour.hpp>
#include <cell/text.hpp>
#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <limits>
#include <numbers>
#include <type_traits>
#include <vector>

namespace cell::ui::widgets {
template<class T> struct Point {
    T x{},y{};
    constexpr Point()=default;
    constexpr Point(T a,T b):x(a),y(b){}
    template<class U> explicit constexpr Point(Point<U> p):x(T(p.x)),y(T(p.y)){}
    T getX()const noexcept{return x;}T getY()const noexcept{return y;}
    Point withX(T v)const{return {v,y};}Point withY(T v)const{return {x,v};}
    Point translated(T a,T b)const{return {T(x+a),T(y+b)};}
    Point operator+(Point p)const{return {T(x+p.x),T(y+p.y)};}
    Point operator-(Point p)const{return {T(x-p.x),T(y-p.y)};}
    Point operator*(T f)const{return {T(x*f),T(y*f)};}
    Point operator/(T f)const{return {T(x/f),T(y/f)};}
    Point& operator+=(Point p){x+=p.x;y+=p.y;return *this;}
    Point& operator-=(Point p){x-=p.x;y-=p.y;return *this;}
    bool operator==(const Point&)const=default;
    Point<float> toFloat()const{return {float(x),float(y)};}
    Point<int> toInt()const{return {int(x),int(y)};}
    double getDistanceFrom(Point p)const{return std::hypot(double(x)-p.x,double(y)-p.y);}
    double getDistanceSquaredFrom(Point p)const{const double a=double(x)-p.x,b=double(y)-p.y;return a*a+b*b;}
    double getAngleToPoint(Point p)const{return std::atan2(double(p.x)-x,double(y)-p.y);}
};
class AffineTransform {
  public:
    double mat00=1,mat01=0,mat02=0,mat10=0,mat11=1,mat12=0;
    AffineTransform()=default;
    AffineTransform(double a,double b,double c,double d,double e,double f)
        :mat00(a),mat01(b),mat02(c),mat10(d),mat11(e),mat12(f){}
    static AffineTransform translation(double x,double y){return {1,0,x,0,1,y};}
    static AffineTransform scale(double x,double y){return {x,0,0,0,y,0};}
    static AffineTransform scale(double v){return scale(v,v);}
    static AffineTransform rotation(double r){const auto c=std::cos(r),s=std::sin(r);return {c,-s,0,s,c,0};}
    static AffineTransform rotation(double r,double x,double y){return translation(-x,-y).followedBy(rotation(r)).translated(x,y);}
    bool isIdentity()const{return mat00==1&&mat01==0&&mat02==0&&mat10==0&&mat11==1&&mat12==0;}
    AffineTransform followedBy(const AffineTransform& n)const {
        return {n.mat00*mat00+n.mat01*mat10,n.mat00*mat01+n.mat01*mat11,n.mat00*mat02+n.mat01*mat12+n.mat02,
                n.mat10*mat00+n.mat11*mat10,n.mat10*mat01+n.mat11*mat11,n.mat10*mat02+n.mat11*mat12+n.mat12};
    }
    AffineTransform translated(double x,double y)const{return followedBy(translation(x,y));}
    AffineTransform scaled(double x,double y)const{return followedBy(scale(x,y));}
    AffineTransform scaled(double v)const{return scaled(v,v);}
    AffineTransform rotated(double v)const{return followedBy(rotation(v));}
    AffineTransform inverted()const {
        const double determinant=mat00*mat11-mat01*mat10;
        if(!std::isfinite(determinant)||std::abs(determinant)<=std::numeric_limits<double>::min())return {};
        return {mat11/determinant,-mat01/determinant,(mat01*mat12-mat11*mat02)/determinant,
                -mat10/determinant,mat00/determinant,(mat10*mat02-mat00*mat12)/determinant};
    }
    template<class T>Point<T> transformedPoint(Point<T> p)const{return {T(mat00*p.x+mat01*p.y+mat02),T(mat10*p.x+mat11*p.y+mat12)};}
    template<class T>void transformPoint(T& x,T& y)const{const auto p=transformedPoint(Point<T>{x,y});x=p.x;y=p.y;}
};
template<class T> class Rectangle {
    T x_{},y_{},w_{},h_{};
  public:
    constexpr Rectangle()=default;
    constexpr Rectangle(T width,T height):w_(std::max(T{},width)),h_(std::max(T{},height)){}
    constexpr Rectangle(T x,T y,T width,T height):x_(x),y_(y),w_(std::max(T{},width)),h_(std::max(T{},height)){}
    Rectangle(Point<T>a,Point<T>b):Rectangle(std::min(a.x,b.x),std::min(a.y,b.y),T(std::abs(b.x-a.x)),T(std::abs(b.y-a.y))){}
    T getX()const{return x_;}T getY()const{return y_;}T getWidth()const{return w_;}T getHeight()const{return h_;}
    T getRight()const{return x_+w_;}T getBottom()const{return y_+h_;}
    T getCentreX()const{return x_+w_/T(2);}T getCentreY()const{return y_+h_/T(2);}
    Point<T> getCentre()const{return {getCentreX(),getCentreY()};}
    Point<T> getPosition()const{return {x_,y_};}Point<T>getTopLeft()const{return {x_,y_};}
    Point<T>getTopRight()const{return {getRight(),y_};}Point<T>getBottomLeft()const{return {x_,getBottom()};}Point<T>getBottomRight()const{return {getRight(),getBottom()};}
    bool isEmpty()const{return w_<=T{}||h_<=T{};}
    bool contains(T x,T y)const{return x>=x_&&y>=y_&&x<getRight()&&y<getBottom();}
    bool contains(Point<T> p)const{return contains(p.x,p.y);}
    bool contains(Rectangle r)const{return r.x_>=x_&&r.y_>=y_&&r.getRight()<=getRight()&&r.getBottom()<=getBottom();}
    bool intersects(Rectangle r)const{return r.getRight()>x_&&r.getBottom()>y_&&r.x_<getRight()&&r.y_<getBottom();}
    bool operator==(const Rectangle&)const=default;
    void setBounds(T x,T y,T w,T h){*this={x,y,w,h};}
    void setX(T x){x_=x;}void setY(T y){y_=y;}void setWidth(T w){w_=std::max(T{},w);}void setHeight(T h){h_=std::max(T{},h);}
    void setPosition(T x,T y){x_=x;y_=y;}void setPosition(Point<T>p){setPosition(p.x,p.y);}
    void setSize(T w,T h){setWidth(w);setHeight(h);}void setCentre(T x,T y){x_=x-w_/T(2);y_=y-h_/T(2);}
    void setLeft(T x){w_=std::max(T{},getRight()-x);x_=x;}void setTop(T y){h_=std::max(T{},getBottom()-y);y_=y;}
    void setRight(T x){w_=std::max(T{},x-x_);}void setBottom(T y){h_=std::max(T{},y-y_);}
    Rectangle withX(T x)const{return {x,y_,w_,h_};}Rectangle withY(T y)const{return {x_,y,w_,h_};}
    Rectangle withPosition(T x,T y)const{return {x,y,w_,h_};}Rectangle withPosition(Point<T>p)const{return withPosition(p.x,p.y);}
    Rectangle withLeft(T v)const{auto r=*this;r.setLeft(v);return r;}
    Rectangle withTop(T v)const{auto r=*this;r.setTop(v);return r;}
    Rectangle withRight(T v)const{auto r=*this;r.setRight(v);return r;}
    Rectangle withBottom(T v)const{auto r=*this;r.setBottom(v);return r;}
    Rectangle withWidth(T w)const{return {x_,y_,w,h_};}Rectangle withHeight(T h)const{return {x_,y_,w_,h};}
    Rectangle withSize(T w,T h)const{return {x_,y_,w,h};}
    Rectangle withSizeKeepingCentre(T w,T h)const{return {getCentreX()-w/T(2),getCentreY()-h/T(2),w,h};}
    Rectangle withCentre(Point<T>p)const{return {p.x-w_/T(2),p.y-h_/T(2),w_,h_};}
    Rectangle translated(T x,T y)const{return {x_+x,y_+y,w_,h_};}
    void translate(T x,T y){x_+=x;y_+=y;}
    Rectangle reduced(T x,T y)const{return {x_+x,y_+y,std::max(T{},w_-T(2)*x),std::max(T{},h_-T(2)*y)};}
    Rectangle reduced(T v)const{return reduced(v,v);}Rectangle expanded(T x,T y)const{return reduced(-x,-y);}Rectangle expanded(T v)const{return expanded(v,v);}
    void reduce(T x,T y){*this=reduced(x,y);}void expand(T x,T y){*this=expanded(x,y);}
    Rectangle withTrimmedLeft(T n)const{return {x_+n,y_,std::max(T{},w_-n),h_};}
    Rectangle withTrimmedRight(T n)const{return {x_,y_,std::max(T{},w_-n),h_};}
    Rectangle withTrimmedTop(T n)const{return {x_,y_+n,w_,std::max(T{},h_-n)};}
    Rectangle withTrimmedBottom(T n)const{return {x_,y_,w_,std::max(T{},h_-n)};}
    Rectangle removeFromLeft(T n){n=std::clamp(n,T{},w_);auto r=Rectangle{x_,y_,n,h_};x_+=n;w_-=n;return r;}
    Rectangle removeFromRight(T n){n=std::clamp(n,T{},w_);auto r=Rectangle{getRight()-n,y_,n,h_};w_-=n;return r;}
    Rectangle removeFromTop(T n){n=std::clamp(n,T{},h_);auto r=Rectangle{x_,y_,w_,n};y_+=n;h_-=n;return r;}
    Rectangle removeFromBottom(T n){n=std::clamp(n,T{},h_);auto r=Rectangle{x_,getBottom()-n,w_,n};h_-=n;return r;}
    Rectangle getIntersection(Rectangle r)const{const auto x=std::max(x_,r.x_),y=std::max(y_,r.y_);return {x,y,std::max(T{},std::min(getRight(),r.getRight())-x),std::max(T{},std::min(getBottom(),r.getBottom())-y)};}
    Rectangle getUnion(Rectangle r)const{if(isEmpty())return r;if(r.isEmpty())return *this;const auto x=std::min(x_,r.x_),y=std::min(y_,r.y_);return {x,y,std::max(getRight(),r.getRight())-x,std::max(getBottom(),r.getBottom())-y};}
    Rectangle constrainedWithin(Rectangle area)const{const auto width=std::min(w_,area.w_),height=std::min(h_,area.h_);return {std::clamp(x_,area.x_,area.getRight()-width),std::clamp(y_,area.y_,area.getBottom()-height),width,height};}
    Rectangle<float>toFloat()const{return {float(x_),float(y_),float(w_),float(h_)};}
    Rectangle<double>toDouble()const{return {double(x_),double(y_),double(w_),double(h_)};}
    Rectangle<int>toNearestInt()const{return {int(std::round(x_)),int(std::round(y_)),int(std::round(getRight()))-int(std::round(x_)),int(std::round(getBottom()))-int(std::round(y_))};}
    Rectangle<int>getSmallestIntegerContainer()const{return {int(std::floor(x_)),int(std::floor(y_)),int(std::ceil(getRight()))-int(std::floor(x_)),int(std::ceil(getBottom()))-int(std::floor(y_))};}
    Rectangle transformedBy(const AffineTransform& t)const{const auto a=t.transformedPoint(getTopLeft()),b=t.transformedPoint(getTopRight()),c=t.transformedPoint(getBottomLeft()),d=t.transformedPoint(getBottomRight());const auto x=std::min({a.x,b.x,c.x,d.x}),y=std::min({a.y,b.y,c.y,d.y});return {x,y,std::max({a.x,b.x,c.x,d.x})-x,std::max({a.y,b.y,c.y,d.y})-y};}
};
template<class T>struct Line {
    Point<T>start,end;
    Line()=default;Line(T a,T b,T c,T d):start(a,b),end(c,d){}Line(Point<T>a,Point<T>b):start(a),end(b){}
    Point<T>getStart()const{return start;}Point<T>getEnd()const{return end;}
};
template<class T>class BorderSize {
    T top_{},left_{},bottom_{},right_{};
  public:
    BorderSize()=default;explicit BorderSize(T all):top_(all),left_(all),bottom_(all),right_(all){}
    BorderSize(T top,T left,T bottom,T right):top_(top),left_(left),bottom_(bottom),right_(right){}
    T getTop()const{return top_;}T getLeft()const{return left_;}T getBottom()const{return bottom_;}T getRight()const{return right_;}
    Rectangle<T>subtractedFrom(Rectangle<T>r)const{return {T(r.getX()+left_),T(r.getY()+top_),std::max(T{},T(r.getWidth()-left_-right_)),std::max(T{},T(r.getHeight()-top_-bottom_))};}
};
using Colour=::cell::Colour;
namespace Colours {
inline constexpr Colour transparentBlack{0x00000000u},transparentWhite{0x00ffffffu},black{0xff000000u},white{0xffffffffu},grey{0xff808080u},darkgrey{0xff404040u},lightgrey{0xffd3d3d3u},red{0xffff0000u},green{0xff008000u},blue{0xff0000ffu},orange{0xffffa500u},yellow{0xffffff00u},cyan{0xff00ffffu};
}
struct Justification {
    enum Flags {left=1,right=2,horizontallyCentred=4,top=8,bottom=16,verticallyCentred=32,
                centred=horizontallyCentred|verticallyCentred,centredLeft=left|verticallyCentred,
                centredRight=right|verticallyCentred,centredTop=horizontallyCentred|top,
                topLeft=left|top,topRight=right|top};
    int flags=centredLeft;
    Justification()=default;Justification(int f):flags(f){}
    int getFlags()const{return flags;}
};
} // namespace cell::ui::widgets
