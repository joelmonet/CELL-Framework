// Original Cell Framework implementation; see LICENSE.
#include <cell/widgets/drawing.hpp>
#include <cmath>
#include <iostream>
#include <stdexcept>

namespace {
void check(bool v,const char*m){if(!v)throw std::runtime_error(m);}
}
int main(){using namespace cell::ui::widgets;try{
    Rectangle<int> area{10,20,300,180};const auto left=area.removeFromLeft(40);check(left==Rectangle<int>{10,20,40,180}&&area.getX()==50&&area.getWidth()==260,"Native layout consumed the wrong rectangle");
    const auto transform=AffineTransform::translation(-4,7).scaled(1.25,1.5).rotated(.17).translated(20,-8);
    for(int n=0;n<1000;++n){Point<double>p{n*.31-120,n*.19+6};const auto roundtrip=transform.inverted().transformedPoint(transform.transformedPoint(p));check(roundtrip.getDistanceFrom(p)<1.e-9,"Native input/drawing transforms disagreed");}
    Image image(Image::ARGB,320,180,true);
    {Graphics g(image);g.fillAll(Colour{0xff101820});g.setColour(Colour{0xffa8d989});g.fillRoundedRectangle(10,10,100,40,6);
     g.setGradientFill({Colour{0xffff8000},20,80,Colour{0xff0080ff},140,80,false});Path blob;blob.addRoundedRectangle(20,60,120,50,8);g.fillPath(blob);
     g.setColour(Colours::white);g.setFont(Font{13,Font::bold});g.drawText("Band Aide",Rectangle<int>{10,120,180,40},Justification::centredLeft);}
    check(image.getPixelAt(20,20).getARGB()==0xffa8d989,"Native rounded controls did not paint their actual color");
    const auto a=image.getPixelAt(30,80),b=image.getPixelAt(130,80);check(a.getRed()>b.getRed()&&a.getBlue()<b.getBlue(),"Native blob gradient was lost or reversed");
    check(Font{13}.getAscent()>0&&Font{13}.getDescent()>0&&Font{13}.getStringWidthFloat("Band Aide")>20,"Native font metrics are unavailable");
    Image source(Image::ARGB,8,4,true);for(int x=0;x<8;++x)for(int y=0;y<4;++y)source.setPixelAt(x,y,x<4?Colours::red:Colours::blue);
    Image crop(Image::ARGB,40,40,true);{Graphics g(crop);g.setImageResamplingQuality(Graphics::lowResamplingQuality);g.drawImage(source,0,0,40,40,4,0,4,4);}
    check(crop.getPixelAt(0,0)==Colours::blue&&crop.getPixelAt(39,39)==Colours::blue,"Native image cropping sampled the wrong source region");
    auto copy=crop.createCopy();crop.setPixelAt(0,0,Colours::red);check(copy.getPixelAt(0,0)==Colours::blue,"Independent image copy shared mutable pixels");
    cell::ui::Surface surface;surface.resize(24,24);bool rejected=false;
    const auto*originalPixels=surface.pixels();
    surface.render([&](cell::ui::Canvas&){surface.resize(24,24);});
    check(surface.pixels()==originalPixels,"Idempotent layout replaced a live raster");
    try{surface.render([&](cell::ui::Canvas&){surface.resize(30,30);});}catch(const std::logic_error&){rejected=true;}
    check(rejected,"Surface replacement invalidated a live painter");
    surface.render([](cell::ui::Canvas&c){c.fill({0,0,24,24},cell::ui::Color{0xffa8d989});});
    check(surface.width()==24&&surface.pixels()!=nullptr,"An interrupted paint poisoned later frames");
    std::cout<<"PASS Cell native drawing/layout: coordinate roundtrips, controls, blob gradients, actual font metrics, cropped images, copy ownership and exception-safe paint lifetimes\n";
}catch(const std::exception&e){std::cerr<<e.what()<<'\n';return 1;}}
