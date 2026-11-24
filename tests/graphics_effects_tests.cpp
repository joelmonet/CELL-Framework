#include <cell/graphics_effects.hpp>
#include "allocation_watch.hpp"
#include <algorithm>
#include <chrono>
#include <cmath>
#include <iostream>
#include <limits>
#include <stdexcept>
#include <vector>
using namespace cell::graphics;
namespace {
void check(bool v, const char* msg) { if (!v) throw std::runtime_error(msg); }
template<class F> void rejects(F f) { bool caught=false; try { f(); } catch(const std::invalid_argument&) { caught=true; } check(caught,"invalid graphics request accepted"); }
// Slow independent direct convolution, including fractional fringe taps. No
// sliding sum or production helper is used to calculate the expected pixels.
std::vector<double> reference(std::vector<double> src, int w, int h, double sigma, EdgeMode edge) {
    const int r = int(std::floor((std::sqrt(1+4*sigma*sigma)-1)/2));
    const double v=sigma*sigma/3, base=double(r)*(r+1)*(2*r+1)/3;
    const double f=std::clamp((v*(2*r+1)-base)/(2*((r+1.)*(r+1)-v)),0.,1.);
    std::vector<double> dst(src.size());
    for(int n=0;n<6;++n) {
        for(int y=0;y<h;++y) for(int x=0;x<w;++x) {
            double total=0;
            for(int tap=-r-1;tap<=r+1;++tap) {
                int xx=x+(n%2 ? 0:tap), yy=y+(n%2 ? tap:0);
                const double weight=std::abs(tap)==r+1?f:1;
                if(edge==EdgeMode::transparent&&(xx<0||xx>=w||yy<0||yy>=h)) continue;
                xx=std::clamp(xx,0,w-1); yy=std::clamp(yy,0,h-1);
                total+=src[std::size_t(yy)*w+xx]*weight;
            }
            dst[std::size_t(y)*w+x]=total/(2*r+1+2*f);
        }
        src.swap(dst);
    }
    return src;
}
void correctness() {
    BlurWorkspace workspace;
    for(auto [w,h]: {std::pair{1,1},{1,19},{23,1},{13,17}})
    for(double sigma: {0.,.1,.5,.999,1.,1.001,2.,7.3,256.})
    for(auto edge: {EdgeMode::transparent,EdgeMode::clamp}) {
        std::vector<std::uint8_t> pixels(std::size_t(w)*h);
        for(std::size_t i=0;i<pixels.size();++i) pixels[i]=std::uint8_t((i*101+37)%256);
        const auto expected=reference({pixels.begin(),pixels.end()},w,h,sigma,edge);
        workspace.alpha(pixels,w,h,sigma,edge);
        for(std::size_t i=0;i<pixels.size();++i) check(std::abs(int(pixels[i])-std::lround(expected[i]))<=1,"sliding blur differs from direct convolution");
    }
    std::vector<std::uint8_t> constant(73*41,123);
    workspace.alpha(constant,73,41,61,EdgeMode::clamp);
    check(std::all_of(constant.begin(),constant.end(),[](auto a){return a==123;}),"clamp changed constant image");
    std::vector<std::uint8_t> square(129*129);
    for(int y=32;y<97;++y)for(int x=32;x<97;++x)square[y*129+x]=255;
    auto before=square, after=square;
    workspace.alpha(before,129,129,7.999);workspace.alpha(after,129,129,8.001);
    for(std::size_t i=0;i<before.size();++i)check(std::abs(int(before[i])-int(after[i]))<=1,"blur radius has visible integer step");
    for(int y=0;y<129;++y)for(int x=0;x<129;++x)check(before[y*129+x]==before[(128-y)*129+128-x],"blur lost mirror symmetry");
    // Half-plane blurred by an actual Gaussian has an erf profile. This checks
    // approximation quality rather than only repeating the box implementation.
    std::vector<std::uint8_t> half(257*33);
    for(int y=0;y<33;++y)for(int x=128;x<257;++x)half[y*257+x]=255;
    workspace.alpha(half,257,33,8,EdgeMode::clamp);
    double maxError=0;
    for(int x=95;x<160;++x) {
        const double gaussian=127.5*(1+std::erf((x-127.5)/(8*std::sqrt(2.))));
        maxError=std::max(maxError,std::abs(half[16*257+x]-gaussian));
    }
    check(maxError<6,"Gaussian approximation exceeds 6/255 half-plane error");
    std::cout<<"Gaussian half-plane max error: "<<maxError<<" / 255\n";
    std::vector<std::uint32_t> rgba(31*29);
    for(std::size_t i=0;i<rgba.size();++i){unsigned a=unsigned(i*43%256);rgba[i]=(a<<24)|((a/2)<<16)|((a/3)<<8)|a;}
    workspace.argb(rgba,31,29,3.7);
    for(auto c:rgba) check(((c>>16)&255)<=(c>>24)&&((c>>8)&255)<=(c>>24)&&(c&255)<=(c>>24),"blur produced non-premultiplied color fringe");
}
void contracts() {
    BlurWorkspace workspace;std::vector<std::uint8_t> image(64,77);const auto original=image;
    for(double s: {-1.,257.,std::numeric_limits<double>::infinity(),std::numeric_limits<double>::quiet_NaN()})rejects([&]{workspace.alpha(image,8,8,s);});
    rejects([&]{workspace.alpha(image,7,8,1);});rejects([&]{workspace.alpha(image,0,8,1);});
    rejects([]{checkedPixelCount(8193,1);});rejects([]{checkedPixelCount(8192,8192);});
    check(image==original&&workspace.retainedBytes()==0,"invalid request mutated pixels or allocated scratch");
    workspace.alpha(image,8,8,2);const auto bytes=workspace.retainedBytes();
    workspace.alpha(image,8,8,3);check(workspace.retainedBytes()==bytes,"fixed-size blur grew scratch on radius change");
    check(blurPadding(0)==0&&blurPadding(1)==3,"padding excludes filter support");
    workspace.release();check(workspace.retainedBytes()==0,"scratch release retained storage");
    for(int fail : {0,1}) {
        BlurWorkspace fresh; auto untouched=original;
        cell_test::failAfter=fail; bool failed=false;
        try { fresh.alpha(untouched,8,8,2); } catch(const std::bad_alloc&) { failed=true; }
        cell_test::failAfter=-1;
        check(failed && untouched==original,"scratch allocation failure changed alpha input");
        std::vector<std::uint32_t> rgba(64,0xff123456), saved=rgba;
        BlurWorkspace colorFresh; cell_test::failAfter=fail; failed=false;
        try { colorFresh.argb(rgba,8,8,2); } catch(const std::bad_alloc&) { failed=true; }
        cell_test::failAfter=-1;
        check(failed && rgba==saved,"scratch allocation failure changed ARGB input");
    }
}
}
int main(){try{correctness();contracts();std::cout<<"PASS: blur direct reference, Gaussian profile, continuity, premultiplication and bounds\n";}catch(const std::exception& e){std::cerr<<e.what()<<'\n';return 1;}}
