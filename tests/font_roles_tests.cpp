// Original Cell Framework implementation; see LICENSE.
#include <cell/widgets/drawing.hpp>
#include <iostream>
namespace w=cell::ui::widgets;using namespace cell::ui;
namespace {void need(bool value,const char*message){if(!value)throw std::runtime_error(message);}}
int main(){try{
    Surface surface;surface.resize(720,250);double normal{},grotesk{},condensed{};
    surface.render([&](Canvas&canvas){
        canvas.fill({0,0,720,250},Color{0xff141719});
        normal=canvas.textWidth("Band Aide",36,600);
        grotesk=canvas.textWidth("Band Aide",36,600,Typeface::brandGrotesk);
        condensed=canvas.textWidth("Band Aide",36,600,Typeface::brandCondensed);
        need(normal>0&&grotesk>0&&condensed>0&&std::abs(grotesk-normal)>.1&&std::abs(condensed-grotesk)>.1,"Brand fonts silently fell back to the interface face");
        for(auto role:{Typeface::brandGrotesk,Typeface::brandCondensed})need(canvas.missingGlyphs("Band Aide 0123456789",role)==0,"Brand face is missing logo/value glyphs");
        canvas.textWeighted({24,50},"Band Aide",36,Color{0xffa9d783},600,Typeface::brandGrotesk);
        canvas.textWeighted({24,108},"Performance Suite",26,Color{0xffe8ece8},600,Typeface::brandCondensed);
        canvas.textWeighted({24,160},"École — Straße — Ελληνικά",18,Color{0xffe8ece8},400);
        need(canvas.missingGlyphs("声",Typeface::brandGrotesk)>0,"Coverage API concealed unsupported language glyphs");
    });
    const auto font=w::Font(36,w::Font::bold).withTypeface(Typeface::brandGrotesk);
    need(std::abs(font.getStringWidthFloat("Band Aide")-grotesk)<.01,"Widget metrics and rendering use different brand faces");
    const auto*rows=static_cast<const unsigned char*>(surface.pixels());bool painted=false;
    for(int y=0;y<surface.height();++y){const auto*row=reinterpret_cast<const std::uint32_t*>(rows+std::size_t(y)*surface.stride());for(int x=0;x<surface.width();++x)painted|=row[x]!=0xff141719;}
    need(painted,"Brand font rendering produced no visible text");
    std::cout<<"PASS shared font roles: distinct licensed brand faces, actual glyph coverage, painted output and matching widget metrics\n";return 0;
}catch(const std::exception&error){std::cerr<<error.what()<<'\n';return 1;}}
