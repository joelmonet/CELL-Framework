// Original Cell Framework implementation; see LICENSE.
#pragma once
#include <cell/ui.hpp>
#include <cell/widgets/geometry.hpp>
#include <cell/widgets/text.hpp>
#include <memory>
#include <optional>
#include <tuple>

namespace cell::ui::widgets {
class Font {
    float size_=12,weight_=400;Typeface typeface_=Typeface::interfaceText;
  public:
    enum {plain=0,bold=1,italic=2};
    Font()=default;explicit Font(float size,int flags=plain):size_(size),weight_((flags&bold)?700.f:400.f){}
    Font withHeight(float h)const{auto f=*this;f.size_=h;return f;}Font boldened()const{auto f=*this;f.weight_=700;return f;}
    Font withTypeface(Typeface face)const{auto result=*this;result.typeface_=face;return result;}Typeface typeface()const noexcept{return typeface_;}
    float getHeight()const noexcept{return size_;}float weight()const noexcept{return weight_;}
    float getAscent()const;float getDescent()const;
    float getStringWidthFloat(const Text&)const;int getStringWidth(const Text& t)const{return int(std::ceil(getStringWidthFloat(t)));}
};
class Path {
    std::vector<VectorCommand> commands_;
  public:
    std::span<const VectorCommand>commands()const{return commands_;}
    void clear(){commands_.clear();}bool isEmpty()const{return commands_.empty();}
    void preallocateSpace(int n){if(n>0)commands_.reserve(std::size_t(n));}
    void startNewSubPath(float x,float y){commands_.push_back({VectorCommand::Kind::move,{x,y},{},{}});}
    void startNewSubPath(Point<float>p){startNewSubPath(p.x,p.y);}
    void lineTo(float x,float y){commands_.push_back({VectorCommand::Kind::line,{x,y},{},{}});}
    void lineTo(Point<float>p){lineTo(p.x,p.y);}
    void quadraticTo(float a,float b,float c,float d){commands_.push_back({VectorCommand::Kind::quadratic,{a,b},{c,d},{}});}
    void cubicTo(float a,float b,float c,float d,float e,float f){commands_.push_back({VectorCommand::Kind::cubic,{a,b},{c,d},{e,f}});}
    void closeSubPath(){commands_.push_back({VectorCommand::Kind::close,{},{},{}});}
    void addRectangle(float x,float y,float w,float h){startNewSubPath(x,y);lineTo(x+w,y);lineTo(x+w,y+h);lineTo(x,y+h);closeSubPath();}
    template<class T>void addRectangle(Rectangle<T>r){addRectangle(float(r.getX()),float(r.getY()),float(r.getWidth()),float(r.getHeight()));}
    void addTriangle(float a,float b,float c,float d,float e,float f){startNewSubPath(a,b);lineTo(c,d);lineTo(e,f);closeSubPath();}
    void addRoundedRectangle(float x,float y,float w,float h,float radius);
    template<class T>void addRoundedRectangle(Rectangle<T>r,float radius){addRoundedRectangle(float(r.getX()),float(r.getY()),float(r.getWidth()),float(r.getHeight()),radius);}
    void addEllipse(float x,float y,float w,float h);
    template<class T>void addEllipse(Rectangle<T>r){addEllipse(float(r.getX()),float(r.getY()),float(r.getWidth()),float(r.getHeight()));}
    void addStar(Point<float>centre,int points,float inner,float outer,float angle=0);
    void addArc(float x,float y,float w,float h,float start,float end,bool begin=false);
    void applyTransform(const AffineTransform&);
    Rectangle<float>getBounds()const;
    void scaleToFit(float x,float y,float w,float h,bool preserve);
};
class PathStrokeType {
  public:
    enum JointStyle {mitered,curved,beveled};enum EndCapStyle {butt,square,rounded};
    explicit PathStrokeType(float thickness,JointStyle join=mitered,EndCapStyle cap=butt):thickness_(thickness),join_(join),cap_(cap){}
    float getStrokeThickness()const{return thickness_;}bool isRounded()const{return join_==curved||cap_==rounded;}
  private:float thickness_;JointStyle join_;EndCapStyle cap_;
};
struct ColourGradient {
    Colour first,second;Point<float>start,end;bool radial{};
    ColourGradient()=default;
    ColourGradient(Colour a,float x,float y,Colour b,float x2,float y2,bool r):first(a),second(b),start(x,y),end(x2,y2),radial(r){}
    ColourGradient(Colour a,Point<float>p,Colour b,Point<float>q,bool r):first(a),second(b),start(p),end(q),radial(r){}
};
struct SoftwareImageType {};
class Image {
    std::shared_ptr<Surface> surface_;
  public:
    enum PixelFormat{UnknownFormat,RGB,ARGB,SingleChannel};
    Image()=default;
    Image(PixelFormat,int width,int height,bool clear=true);
    Image(PixelFormat format,int width,int height,bool clear,SoftwareImageType):Image(format,width,height,clear){}
    bool isValid()const noexcept{return bool(surface_);}bool isNull()const noexcept{return !surface_;}
    int getWidth()const noexcept{return surface_?surface_->width():0;}int getHeight()const noexcept{return surface_?surface_->height():0;}
    Rectangle<int>getBounds()const noexcept{return {0,0,getWidth(),getHeight()};}
    Surface&surface(){return *surface_;}const Surface&surface()const{return *surface_;}
    Image createCopy()const;Image rescaled(int,int)const;
    Colour getPixelAt(int,int)const;void setPixelAt(int,int,Colour);
    bool operator==(const Image&b)const{return surface_==b.surface_;}
};
class Graphics {
    std::unique_ptr<Surface::PaintSession> session_;
    Canvas* canvas_{};Colour colour_{0xff000000};Font font_;
    Rectangle<float> clip_;std::optional<ColourGradient> gradient_;
    std::vector<std::tuple<Colour,Font,Rectangle<float>,std::optional<ColourGradient>>> states_;
    static cell::ui::Rect rect(Rectangle<float>r){return {r.getX(),r.getY(),r.getWidth(),r.getHeight()};}
    void text(std::string_view,Rectangle<float>,Justification,bool,float minimumScale=1.);
  public:
    enum ResamplingQuality{lowResamplingQuality,mediumResamplingQuality,highResamplingQuality};
    explicit Graphics(Image&);Graphics(Canvas&,Rectangle<float> bounds);
    Graphics(const Graphics&)=delete;Graphics&operator=(const Graphics&)=delete;
    class ScopedSaveState {Graphics&g_;public:explicit ScopedSaveState(Graphics&g):g_(g){g_.saveState();}~ScopedSaveState(){g_.restoreState();}};
    void saveState();void restoreState();
    void setColour(Colour c){colour_=c;gradient_.reset();}void setFont(Font f){font_=f;}void setFont(float s){font_=Font{s};}Font getCurrentFont()const{return font_;}
    void setOpacity(float v){opacity_=std::clamp(v,0.f,1.f);canvas_->opacity(opacity_);}
    void multiplyOpacity(float v){setOpacity(opacity_*v);}void setGradientFill(ColourGradient g){gradient_=g;}
    void fillAll(){fillRect(clip_);}void fillAll(Colour c){auto saved=colour_;setColour(c);fillAll();colour_=saved;}
    void fillRect(float x,float y,float w,float h);void fillRect(int x,int y,int w,int h){fillRect(float(x),float(y),float(w),float(h));}
    template<class T>void fillRect(Rectangle<T>r){fillRect(float(r.getX()),float(r.getY()),float(r.getWidth()),float(r.getHeight()));}
    void drawRect(float,float,float,float,float=1.f);
    template<class T>void drawRect(Rectangle<T>r,float thickness=1.f){drawRect(float(r.getX()),float(r.getY()),float(r.getWidth()),float(r.getHeight()),thickness);}
    void fillRoundedRectangle(float,float,float,float,float);
    template<class T>void fillRoundedRectangle(Rectangle<T>r,float radius){fillRoundedRectangle(float(r.getX()),float(r.getY()),float(r.getWidth()),float(r.getHeight()),radius);}
    void drawRoundedRectangle(float,float,float,float,float,float);
    template<class T>void drawRoundedRectangle(Rectangle<T>r,float radius,float thickness){drawRoundedRectangle(float(r.getX()),float(r.getY()),float(r.getWidth()),float(r.getHeight()),radius,thickness);}
    void fillEllipse(float,float,float,float);void drawEllipse(float,float,float,float,float);
    template<class T>void fillEllipse(Rectangle<T>r){fillEllipse(float(r.getX()),float(r.getY()),float(r.getWidth()),float(r.getHeight()));}
    template<class T>void drawEllipse(Rectangle<T>r,float thickness){drawEllipse(float(r.getX()),float(r.getY()),float(r.getWidth()),float(r.getHeight()),thickness);}
    void drawLine(float x,float y,float x2,float y2,float thickness=1.f){canvas_->line({x,y},{x2,y2},Color{colour_.getARGB()},thickness);}
    void drawLine(Line<float>line,float thickness=1.f){drawLine(line.start.x,line.start.y,line.end.x,line.end.y,thickness);}
    void drawArrow(Line<float>,float lineThickness,float headWidth,float headLength);
    void drawHorizontalLine(int y,float x,float x2){fillRect(x,float(y),x2-x,1.f);}void drawVerticalLine(int x,float y,float y2){fillRect(float(x),y,1.f,y2-y);}
    void fillPath(const Path&p,const AffineTransform& t={});void strokePath(const Path&,const PathStrokeType&,const AffineTransform& t={});
    template<class S,class T>void drawText(const S&s,Rectangle<T>r,Justification j,bool ellipses=true){text(textView(s),r.toFloat(),j,ellipses);}
    template<class S>void drawText(const S&s,int x,int y,int w,int h,Justification j,bool ellipses=true){drawText(s,Rectangle<int>{x,y,w,h},j,ellipses);}
    template<class S>void drawFittedText(const S&s,Rectangle<int>r,Justification j,int,float minimumScale=.8f){text(textView(s),r.toFloat(),j,true,minimumScale);}
    bool reduceClipRegion(Rectangle<int>r){clip_=clip_.getIntersection(r.toFloat());canvas_->clip(rect(clip_));return !clip_.isEmpty();}
    bool reduceClipRegion(int x,int y,int w,int h){return reduceClipRegion(Rectangle<int>{x,y,w,h});}
    bool clipRegionIntersects(Rectangle<int>r)const{return clip_.intersects(r.toFloat());}
    Rectangle<int>getClipBounds()const{return clip_.getSmallestIntegerContainer();}
    void setOrigin(int x,int y){canvas_->translate(x,y);clip_=clip_.translated(float(-x),float(-y));}
    void addTransform(const AffineTransform&t){clip_=clip_.transformedBy(t.inverted());canvas_->transform(t.mat00,t.mat01,t.mat02,t.mat10,t.mat11,t.mat12);}
    void setImageResamplingQuality(ResamplingQuality q){quality_=q;}
    void drawImageAt(const Image&,int,int,bool=false);
    void drawImage(const Image&,float,float,float,float,int,int,int,int,bool=false);
    template<class T>void drawImage(const Image&i,Rectangle<T>r){drawImage(i,float(r.getX()),float(r.getY()),float(r.getWidth()),float(r.getHeight()),0,0,i.getWidth(),i.getHeight());}
    void drawImageTransformed(const Image&,const AffineTransform&,bool=false);
  private:float opacity_=1.f;std::vector<float>opacityStack_;ResamplingQuality quality_=mediumResamplingQuality;
};
}
