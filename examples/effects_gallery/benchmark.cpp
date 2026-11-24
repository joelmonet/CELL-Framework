#include <cell/ui_effects.hpp>
#include <algorithm>
#include <array>
#include <chrono>
#include <iostream>
#include <stdexcept>
using namespace cell::ui;
namespace {
struct Result { double micros; EffectCacheStats stats; };
Result run(double scale,bool cached) {
    AlphaMask mask; mask.rasterize(int(256*scale),int(96*scale),[&](Canvas& c){
        c.scale(scale,scale);c.rounded({8,8,240,80},12,Color(0xffffffff));
    });
    Surface output; output.setPixels(std::vector<std::uint32_t>(std::size_t(400*scale)*std::size_t(240*scale)),int(400*scale),int(240*scale));
    EffectCache effects(cached?EffectCacheLimits{}:EffectCacheLimits{0,0});
    ShadowStyle style{8,{0,4},Color(0x90000000)};
    auto painter=output.beginPaint();auto& canvas=painter->canvas();canvas.scale(scale,scale);
    effects.draw(canvas,mask,{50,50},style,scale); // warm fonts, raster pipeline and scratch
    const int frames=cached?400:20;std::array<double,3> samples{};
    for(auto& sample:samples){
        const auto begin=std::chrono::steady_clock::now();
        for(int frame=0;frame<frames;++frame)effects.draw(canvas,mask,{50,50},style,scale);
        sample=std::chrono::duration<double,std::micro>(std::chrono::steady_clock::now()-begin).count()/frames;
    }
    const auto stats=effects.stats();
    if(cached&&(stats.blurBuilds!=1||stats.colorBuilds!=1||stats.hits!=1200))throw std::runtime_error("hot benchmark rebuilt cached effect");
    if(!cached&&(stats.entries||stats.uncachedDraws!=61))throw std::runtime_error("uncached benchmark retained an effect");
    std::sort(samples.begin(),samples.end());return {samples[1],stats};
}
}
int main(){try{
    std::cout<<"{\"scope\":\"CPU shadow draw microbenchmark, not whole-plugin CPU\",\"cases\":[";
    for(double scale:{1.,2.}){
        const auto cold=run(scale,false),hot=run(scale,true);
        if(scale==2)std::cout<<',';
        std::cout<<"{\"scale\":"<<scale<<",\"uncached_us\":"<<cold.micros<<",\"cached_us\":"<<hot.micros
                 <<",\"retained_bytes\":"<<hot.stats.retainedBytes<<",\"scratch_bytes\":"<<hot.stats.scratchBytes
                 <<",\"warm_draws\":1200,\"blur_builds\":"<<hot.stats.blurBuilds<<",\"color_builds\":"<<hot.stats.colorBuilds<<'}';
    }
    std::cout<<"]}\n";
}catch(const std::exception& e){std::cerr<<e.what()<<'\n';return 1;}}
