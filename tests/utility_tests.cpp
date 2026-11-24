// Original Cell Framework implementation; see LICENSE.
#include <cell/utility.hpp>
#include <cell/vector_math.hpp>
#include <array>
#include <iostream>
#include <vector>

namespace {
void need(bool ok,const char* reason){if(!ok)throw std::runtime_error(reason);}
}
int main(){try{
    int current=1;
    try{cell::ScopedValue guard(current,2);need(current==2,"Scoped value not applied");{cell::ScopedValue nested(current,3);need(current==3,"Nested value not applied");}need(current==2,"Nested value not restored");throw 1;}
    catch(int){}need(current==1,"Value not restored during unwinding");
    auto last=cell::Clock::getHighResolutionTicks();
    for(int i=0;i<1000;++i){const auto now=cell::Clock::getHighResolutionTicks();need(now>=last,"Monotonic clock moved backwards");last=now;}
    need(cell::Clock::getHighResolutionTicksPerSecond()==1000000000,"Clock unit changed");
    const auto start=cell::Clock::getHighResolutionTicks();
    const auto ms=cell::Clock::getMillisecondCounterHiRes();
    const auto end=cell::Clock::getHighResolutionTicks();
    need(ms>=double(start)*1.e-6&&ms<=double(end)*1.e-6,"Clock methods have different origins");
    need(cell::math::limit(0,10,-1)==0&&cell::math::limit(0,10,12)==10,"Limit bounds changed");
    need(cell::math::roundToInt(.5)==0&&cell::math::roundToInt(1.5)==2&&cell::math::roundToInt(-1.5)==-2,"Rounding convention changed");
    need(std::abs(cell::math::Constants<double>::twoPi-6.2831853071795864769)<1.e-15,"Math constants lost precision");
    for(std::size_t count:{std::size_t(0),std::size_t(1),std::size_t(127),std::size_t(128),std::size_t(8193)}){
        std::vector<float> source(count),result(count),oracle(count);
        for(std::size_t i=0;i<count;++i){source[i]=float(std::sin(i*.37));oracle[i]=result[i]=float(std::cos(i*.21));}
        for(std::size_t i=0;i<count;++i)oracle[i]+=source[i]*-.73f;
        cell::dsp::Vector::addWithMultiply(result.data(),source.data(),-.73f,count);
        need(result==oracle,"Vector sum differs from scalar PCM oracle");
        cell::dsp::Vector::negate(result.data(),source.data(),count);
        for(std::size_t i=0;i<count;++i)need(result[i]==-source[i],"Polarity flip failed");
        cell::dsp::Vector::multiply(result.data(),-1,count);need(result==source,"In-place gain changed samples");
        cell::dsp::Vector::clear(result.data(),count);
        for(float x:result)need(x==0,"Clear left signal behind");
        cell::dsp::Vector::copy(result.data(),source.data(),count);need(result==source,"Copy changed samples");
    }
    std::array<float,5> overlap{1,2,3,4,5};
    cell::dsp::Vector::addWithMultiply(overlap.data()+1,overlap.data(),2,4);
    need(overlap==std::array<float,5>{1,4,7,10,13},"Overlapping sum corrupted its source");
    cell::dsp::Vector::copy(overlap.data(),overlap.data()+1,4);
    need(overlap==std::array<float,5>{4,7,10,13,13},"Overlapping copy corrupted samples");
    std::cout<<"PASS native utility: scope unwind, monotonic shared clock, rounding and scalar/overlap PCM oracles\n";
    return 0;
}catch(const std::exception& e){std::cerr<<e.what()<<'\n';return 1;}}
