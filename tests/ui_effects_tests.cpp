#include <cell/ui_effects.hpp>
#include <chrono>
#include <cstring>
#include <iostream>
#include <stdexcept>
using namespace cell::ui;
namespace {
void check(bool v,const char* msg){if(!v)throw std::runtime_error(msg);}
std::uint32_t pixel(const Surface& s,int x,int y){std::uint32_t v;std::memcpy(&v,static_cast<const std::byte*>(s.pixels())+y*s.stride()+4*x,4);return v;}
void blank(Surface& s,int w,int h){s.setPixels(std::vector<std::uint32_t>(std::size_t(w)*h),w,h);}
void masksAndCache(){
    AlphaMask mask;mask.rasterize(32,32,[](Canvas& c){c.rounded({2,2,28,28},4,Color(0xffffffff));});
    const auto rev=mask.revision();mask.setPixels(mask.pixels(),32,32);check(mask.revision()==rev,"identical alpha invalidated cache");
    Surface s;blank(s,120,90);EffectCache cache;ShadowStyle style{2,{0,0},Color(0xffff0000)};
    auto draw=[&]{s.render([&](Canvas& c){cache.draw(c,mask,{30,20},style);});};
    draw();auto initial=cache.stats();check(initial.blurBuilds==1&&initial.colorBuilds==1&&initial.entries==1,"cold shadow not cached");
    const auto started=std::chrono::steady_clock::now();
    for(int i=0;i<100;++i){style.offset.x=i%5;draw();}
    const auto elapsed=std::chrono::duration<double,std::micro>(std::chrono::steady_clock::now()-started).count()/100;
    auto hot=cache.stats();check(hot.blurBuilds==1&&hot.colorBuilds==1&&hot.hits==100,"moving shadow regenerated image");
    style.color=Color(0xff00ffff);style.opacity=.7;draw();check(cache.stats().blurBuilds==1&&cache.stats().colorBuilds==2,"tint change reblurred shape");
    style.kind=EffectKind::innerShadow;style.offset={2,1};blank(s,120,90);draw();
    check((pixel(s,46,36)>>24)==0&&pixel(s,29,30)==0&&(pixel(s,34,24)>>24)>0,"inner shadow leaked outside alpha or filled its center");
    style.offset={3,1};draw();check(cache.stats().blurBuilds==1,"inner offset change reblurred shape");
    auto changed=std::vector<std::uint8_t>(mask.pixels().begin(),mask.pixels().end());changed[0]=255;
    mask.setPixels(changed,32,32);draw();check(cache.stats().blurBuilds==2,"mask edit used stale blur");
    cache.setLimits({1,0});check(cache.stats().retainedBytes==0&&cache.stats().entries==0,"budget shrink did not release cached images");
    draw();check(cache.stats().uncachedDraws==1&&cache.stats().entries==0,"zero budget did not render uncached");
    cache.clear();cache.releaseScratch();check(cache.stats().scratchBytes==0,"cache scratch not releasable");
    cache.setLimits({32768,1});draw();style.sigma=3;draw();check(cache.stats().entries==1&&cache.stats().evictions>=1&&cache.stats().retainedBytes<=32768,"LRU exceeded limits");
    bool rejected=false;style.sigma=-1;try{draw();}catch(const std::invalid_argument&){rejected=true;}check(rejected,"negative sigma accepted");
    std::cout<<"Hot shadow draw (100-frame mean, this machine): "<<elapsed<<" us; no reblur/recolor\n";
}
void compositing(){
    AlphaMask mask;std::vector<std::uint8_t> pixels(4*4,255);mask.setPixels(pixels,4,4);
    EffectCache cache;Surface s;blank(s,20,20);
    s.render([&](Canvas& c){c.opacity(.5);cache.draw(c,mask,{4,4},{0,{0,0},Color(0xffff0000)});});
    auto v=pixel(s,5,5);check((v>>24)>=127&&(v>>24)<=128&&((v>>16)&255)==(v>>24),"shadow ignored outer opacity or tint premultiplication");
    blank(s,20,20);s.render([&](Canvas& c){c.clip({0,0,5,20});cache.draw(c,mask,{4,4},{0,{0,0},Color(0xffffffff)});});
    check(pixel(s,4,4)==0xffffffff&&pixel(s,5,4)==0,"effect draw ignored outer clipping");
}
void layers(){
    Surface s;blank(s,140,90);CachedLayer layer;int calls=0;
    auto paint=[&](Canvas& c){++calls;c.fill({0,0,20,14},Color(0xff123456));c.line({0,0},{20,14},Color(0xffffffff));};
    auto draw=[&](Rect r,double scale,std::uint64_t version){s.render([&](Canvas& c){layer.draw(c,r,scale,version,paint);});};
    draw({2,3,20,14},1,7);draw({30,3,20,14},1,7);
    check(calls==1&&layer.paintCount()==1&&pixel(s,31,10)==0xff123456,"moving layer repainted or lost content");
    draw({2,3,20,14},1.25,7);check(calls==2&&layer.retainedBytes()==25*18*4,"DPI change lost ceil-rounded physical size");
    draw({2,3,20,14},1.25,8);layer.invalidate();draw({2,3,20,14},1.25,8);check(calls==4,"content version/invalidate did not repaint");
    // Match a directly rasterized logical layer at three fractional DPI scales.
    for(double scale:{1.25,1.5,2.}){
        const int w=int(std::ceil(20.3*scale)),h=int(std::ceil(14.7*scale));
        Surface target,reference;blank(target,w,h);blank(reference,w,h);CachedLayer fractional;
        auto p=[](Canvas& c){c.rounded({.4,.8,19.5,13.2},2,Color(0xff345678));};
        target.render([&](Canvas& c){c.scale(scale,scale);fractional.draw(c,{0,0,20.3,14.7},scale,1,p);});
        reference.render([&](Canvas& c){c.scale(scale,scale);c.clip({0,0,20.3,14.7});p(c);});
        for(int y=0;y<h;++y)for(int x=0;x<w;++x)check(pixel(target,x,y)==pixel(reference,x,y),"fractional-DPI cache distorted raster");
    }
    bool failed=false;try{s.render([&](Canvas& c){layer.draw(c,{2,3,20,14},1,9,[](Canvas&){throw std::runtime_error("paint failure");});});}catch(const std::runtime_error&){failed=true;}
    check(failed&&layer.paintCount()==4,"failed layer paint committed a new version");
    draw({2,3,20,14},1,9);check(calls==5,"failed paint could not retry");
    CachedLayer moved(std::move(layer));
    s.render([&](Canvas& c){moved.draw(c,{2,3,20,14},1,9,paint);});
    check(calls==5&&moved.paintCount()==5,"moving a layer discarded its completed raster");
    draw({2,3,20,14},1,9);check(calls==6&&layer.paintCount()==1,"moved-from layer could not repaint safely");
    layer=std::move(moved);draw({2,3,20,14},1,9);check(calls==6,"move assignment invalidated transferred layer");
    layer.clear();check(layer.retainedBytes()==0,"clear retained layer surface");
}
}
int main(){try{masksAndCache();compositing();layers();std::cout<<"PASS: effect cache invalidation/budgets, inner shadows, composition and fractional-DPI layers\n";}catch(const std::exception& e){std::cerr<<e.what()<<'\n';return 1;}}
