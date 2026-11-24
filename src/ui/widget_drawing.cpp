// Original Cell Framework implementation; see LICENSE.
#include <cell/widgets/drawing.hpp>
#include <cstring>
#include <stdexcept>

namespace cell::ui::widgets {
namespace {
TextMetrics measure(const Text& text,const Font& font){
    thread_local Surface surface;
    if(!surface.width())surface.resize(1,1);
    TextMetrics result;
    surface.render([&](Canvas& canvas){result=canvas.textMetrics(text.utf8(),font.getHeight(),font.weight(),font.typeface());});
    return result;
}
cell::ui::Rect native(Rectangle<float>r){return {r.getX(),r.getY(),r.getWidth(),r.getHeight()};}
}
float Font::getStringWidthFloat(const Text& text)const{return float(measure(text,*this).advance);}
float Font::getAscent()const{return float(measure("",*this).ascent);}
float Font::getDescent()const{return float(measure("",*this).descent);}
void Path::addStar(Point<float>centre,int points,float inner,float outer,float angle){if(points<2||!std::isfinite(inner)||!std::isfinite(outer)||inner<0||outer<=0)return;const double step=std::acos(-1.)/points;for(int i=0;i<points*2;++i){const double a=angle+i*step,r=(i&1)?inner:outer;const float x=centre.x+float(std::sin(a)*r),y=centre.y-float(std::cos(a)*r);if(i)lineTo(x,y);else startNewSubPath(x,y);}closeSubPath();}
void Path::addRoundedRectangle(float x,float y,float w,float h,float radius){
    const float r=std::clamp(radius,0.f,std::max(0.f,std::min(w,h)*.5f)),k=.5522847498307936f*r;
    startNewSubPath(x+r,y);lineTo(x+w-r,y);cubicTo(x+w-r+k,y,x+w,y+r-k,x+w,y+r);
    lineTo(x+w,y+h-r);cubicTo(x+w,y+h-r+k,x+w-r+k,y+h,x+w-r,y+h);
    lineTo(x+r,y+h);cubicTo(x+r-k,y+h,x,y+h-r+k,x,y+h-r);
    lineTo(x,y+r);cubicTo(x,y+r-k,x+r-k,y,x+r,y);closeSubPath();
}
void Path::addEllipse(float x,float y,float w,float h){
    const float rx=w*.5f,ry=h*.5f,cx=x+rx,cy=y+ry,k=.5522847498307936f;
    startNewSubPath(cx+rx,cy);cubicTo(cx+rx,cy+ry*k,cx+rx*k,cy+ry,cx,cy+ry);
    cubicTo(cx-rx*k,cy+ry,cx-rx,cy+ry*k,cx-rx,cy);cubicTo(cx-rx,cy-ry*k,cx-rx*k,cy-ry,cx,cy-ry);
    cubicTo(cx+rx*k,cy-ry,cx+rx,cy-ry*k,cx+rx,cy);closeSubPath();
}
void Path::addArc(float x,float y,float w,float h,float start,float end,bool begin){
    const int segments=std::max(1,int(std::ceil(std::abs(end-start)/(std::numbers::pi/2.))));
    const double delta=(end-start)/segments,cx=x+w*.5,cy=y+h*.5,rx=w*.5,ry=h*.5;
    const auto point=[&](double a){return Point<float>{float(cx+rx*std::sin(a)),float(cy-ry*std::cos(a))};};
    const auto p=point(start);if(begin||isEmpty())startNewSubPath(p);else lineTo(p);
    for(int i=0;i<segments;++i){const double a=start+i*delta,b=a+delta,k=4./3.*std::tan(delta/4.);const auto p1=point(a),p2=point(b);
        cubicTo(float(p1.x+k*rx*std::cos(a)),float(p1.y+k*ry*std::sin(a)),float(p2.x-k*rx*std::cos(b)),float(p2.y-k*ry*std::sin(b)),p2.x,p2.y);}
}
void Path::applyTransform(const AffineTransform&t){for(auto&c:commands_){const auto apply=[&](cell::ui::Point&p){const auto n=t.transformedPoint(Point<double>{p.x,p.y});p={n.x,n.y};};if(c.kind!=VectorCommand::Kind::close)apply(c.first);if(c.kind==VectorCommand::Kind::quadratic||c.kind==VectorCommand::Kind::cubic)apply(c.second);if(c.kind==VectorCommand::Kind::cubic)apply(c.third);}}
Rectangle<float>Path::getBounds()const{
    double left=std::numeric_limits<double>::infinity(),top=left,right=-left,bottom=-left;
    const auto include=[&](cell::ui::Point p){left=std::min(left,p.x);top=std::min(top,p.y);right=std::max(right,p.x);bottom=std::max(bottom,p.y);};
    for(const auto& c:commands_){if(c.kind!=VectorCommand::Kind::close)include(c.first);if(c.kind==VectorCommand::Kind::quadratic||c.kind==VectorCommand::Kind::cubic)include(c.second);if(c.kind==VectorCommand::Kind::cubic)include(c.third);}
    return std::isfinite(left)?Rectangle<float>{float(left),float(top),float(right-left),float(bottom-top)}:Rectangle<float>{};
}
void Path::scaleToFit(float x,float y,float w,float h,bool preserve){const auto r=getBounds();if(r.isEmpty())return;double sx=w/r.getWidth(),sy=h/r.getHeight();if(preserve)sx=sy=std::min(sx,sy);applyTransform(AffineTransform::translation(-r.getX(),-r.getY()).scaled(sx,sy).translated(x+(w-r.getWidth()*sx)*.5,y+(h-r.getHeight()*sy)*.5));}
Image::Image(PixelFormat,int width,int height,bool clear){if(width<1||height<1)return;surface_=std::make_shared<Surface>();surface_->resize(width,height);if(clear){std::vector<std::uint32_t>zero(std::size_t(width)*std::size_t(height));surface_->setPixels(zero,width,height);}}
Image Image::createCopy()const{if(!surface_)return {};Image copy(ARGB,getWidth(),getHeight(),false);std::vector<std::uint32_t>pixels(std::size_t(getWidth())*getHeight());for(int row=0;row<getHeight();++row)std::memcpy(pixels.data()+std::size_t(row)*getWidth(),static_cast<const std::byte*>(surface_->pixels())+row*surface_->stride(),std::size_t(getWidth())*4);copy.surface_->setPixels(pixels,getWidth(),getHeight());return copy;}
Image Image::rescaled(int w,int h)const{if(!surface_||w<1||h<1)return {};Image result(ARGB,w,h);result.surface_->render([&](Canvas&c){c.image(*surface_,{0,0,double(w),double(h)});});return result;}
Colour Image::getPixelAt(int x,int y)const{if(!surface_||!getBounds().contains(x,y))return Colour{0u};std::uint32_t pixel;std::memcpy(&pixel,static_cast<const std::byte*>(surface_->pixels())+y*surface_->stride()+x*4,4);const auto a=pixel>>24;if(!a)return Colour{0u};const auto channel=[a](unsigned c){return std::min(255u,(c*255u+a/2)/a);};return Colour{(a<<24)|(channel((pixel>>16)&255u)<<16)|(channel((pixel>>8)&255u)<<8)|channel(pixel&255u)};}
void Image::setPixelAt(int x,int y,Colour c){if(!surface_||!getBounds().contains(x,y))return;const auto a=unsigned(c.getAlpha());const auto channel=[a](unsigned c){return (c*a+127u)/255u;};const auto value=(a<<24)|(channel(c.getRed())<<16)|(channel(c.getGreen())<<8)|channel(c.getBlue());std::memcpy(static_cast<std::byte*>(const_cast<void*>(surface_->pixels()))+y*surface_->stride()+x*4,&value,4);}
Graphics::Graphics(Image& image):session_(image.surface().beginPaint()),canvas_(&session_->canvas()),clip_(image.getBounds().toFloat()){}
Graphics::Graphics(Canvas& canvas,Rectangle<float>bounds):canvas_(&canvas),clip_(bounds){}
void Graphics::saveState(){opacityStack_.push_back(opacity_);canvas_->save();states_.emplace_back(colour_,font_,clip_,gradient_);}
void Graphics::restoreState(){if(!opacityStack_.empty()){opacity_=opacityStack_.back();opacityStack_.pop_back();}if(states_.empty())return;auto&[color,font,clip,gradient]=states_.back();colour_=color;font_=font;clip_=clip;gradient_=gradient;canvas_->restore();states_.pop_back();}
void Graphics::fillRect(float x,float y,float w,float h){if(w<=0||h<=0)return;if(gradient_){const auto&g=*gradient_;const std::array<GradientStop,2>stops{{{0,Color{g.first.getARGB()}},{1,Color{g.second.getARGB()}}}};if(g.radial){Path p;p.addRectangle(x,y,w,h);canvas_->pathGradient(p.commands(),stops,{g.start.x,g.start.y},{g.end.x,g.end.y},true);}else canvas_->gradient({x,y,w,h},stops,{g.start.x,g.start.y},{g.end.x,g.end.y});}else canvas_->fill({x,y,w,h},Color{colour_.getARGB()});}
void Graphics::drawRect(float x,float y,float w,float h,float thickness){canvas_->strokeRounded({x,y,w,h},0,Color{colour_.getARGB()},thickness);}
void Graphics::fillRoundedRectangle(float x,float y,float w,float h,float radius){if(gradient_){Path p;p.addRoundedRectangle(x,y,w,h,radius);fillPath(p);}else canvas_->rounded({x,y,w,h},radius,Color{colour_.getARGB()});}
void Graphics::drawRoundedRectangle(float x,float y,float w,float h,float radius,float thickness){canvas_->strokeRounded({x,y,w,h},radius,Color{colour_.getARGB()},thickness);}
void Graphics::fillEllipse(float x,float y,float w,float h){if(gradient_){Path p;p.addEllipse(x,y,w,h);fillPath(p);}else canvas_->ellipse({x,y,w,h},Color{colour_.getARGB()});}
void Graphics::drawEllipse(float x,float y,float w,float h,float thickness){canvas_->ellipse({x,y,w,h},Color{colour_.getARGB()},thickness);}
void Graphics::fillPath(const Path& path,const AffineTransform&t){ScopedSaveState saved(*this);if(!t.isIdentity())addTransform(t);if(gradient_){const auto&g=*gradient_;const std::array<GradientStop,2>stops{{{0,Color{g.first.getARGB()}},{1,Color{g.second.getARGB()}}}};canvas_->pathGradient(path.commands(),stops,{g.start.x,g.start.y},{g.end.x,g.end.y},g.radial);}else canvas_->path(path.commands(),Color{colour_.getARGB()});}
void Graphics::strokePath(const Path&p,const PathStrokeType&stroke,const AffineTransform&t){ScopedSaveState saved(*this);if(!t.isIdentity())addTransform(t);canvas_->path(p.commands(),Color{colour_.getARGB()},stroke.getStrokeThickness(),stroke.isRounded());}
void Graphics::text(std::string_view value,Rectangle<float>bounds,Justification justification,bool ellipses,float minimumScale){
    if(value.empty()||bounds.isEmpty())return;
    ScopedSaveState saved(*this);canvas_->clip(native(bounds));
    const auto metrics=canvas_->textMetrics(value,font_.getHeight(),font_.weight(),font_.typeface());
    float size=font_.getHeight();double width=metrics.advance;
    if(width>bounds.getWidth()&&minimumScale<1.){const double scale=std::max(double(minimumScale),bounds.getWidth()/width);size=float(size*scale);width*=scale;}
    std::string shortened;
    if(width>bounds.getWidth()&&ellipses){shortened.assign(value);const auto suffix=std::string("…");const auto suffixWidth=canvas_->textWidth(suffix,size,font_.weight(),font_.typeface());while(!shortened.empty()&&canvas_->textWidth(shortened,size,font_.weight(),font_.typeface())+suffixWidth>bounds.getWidth()){auto n=shortened.size()-1;while(n&&((static_cast<unsigned char>(shortened[n])&0xc0)==0x80))--n;shortened.resize(n);}shortened+=suffix;value=shortened;width=canvas_->textWidth(value,size,font_.weight(),font_.typeface());}
    double x=bounds.getX();if(justification.flags&Justification::right)x=bounds.getRight()-width;else if(justification.flags&Justification::horizontallyCentred)x=bounds.getCentreX()-width*.5;
    const auto line=canvas_->textMetrics("",size,font_.weight(),font_.typeface());double baseline=bounds.getY()+line.ascent;if(justification.flags&Justification::bottom)baseline=bounds.getBottom()-line.descent;else if(justification.flags&Justification::verticallyCentred)baseline=bounds.getCentreY()+(line.ascent-line.descent)*.5;
    canvas_->textWeighted({x,baseline},value,size,Color{colour_.getARGB()},font_.weight(),font_.typeface());
}
void Graphics::drawImageAt(const Image&i,int x,int y,bool){if(i.isValid())canvas_->image(i.surface(),{double(x),double(y),double(i.getWidth()),double(i.getHeight())});}
void Graphics::drawImage(const Image&i,float x,float y,float w,float h,int sx,int sy,int sw,int sh,bool){if(i.isValid())canvas_->image(i.surface(),{x,y,w,h},{double(sx),double(sy),double(sw),double(sh)},quality_==lowResamplingQuality);}
void Graphics::drawImageTransformed(const Image&i,const AffineTransform&t,bool){ScopedSaveState saved(*this);addTransform(t);drawImageAt(i,0,0);}
void Graphics::drawArrow(Line<float>line,float thickness,float headWidth,float headLength){const float dx=line.end.x-line.start.x,dy=line.end.y-line.start.y,length=std::hypot(dx,dy);if(length<=0)return;const float ux=dx/length,uy=dy/length,l=std::min(std::max(0.f,headLength),length);const Point<float>base{line.end.x-ux*l,line.end.y-uy*l};drawLine(line.start.x,line.start.y,base.x,base.y,thickness);Path head;head.addTriangle(line.end.x,line.end.y,base.x-uy*headWidth*.5f,base.y+ux*headWidth*.5f,base.x+uy*headWidth*.5f,base.y-ux*headWidth*.5f);fillPath(head);}

}
