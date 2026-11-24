// SPDX-License-Identifier: Apache-2.0
#include <cell/filter_slopes.hpp>
#include <complex>
#include <iostream>
#include <stdexcept>
using namespace cell;
void check(bool value,const char* why){if(!value)throw std::runtime_error(why);}
double response(double rate,double hz,double cutoff,int order,bool hp){
 const auto z=std::polar(1.,-2*std::numbers::pi*hz/rate);std::complex<double> result{1,0};
 for(int i=0;i<filters::sections(order);++i){const auto c=filters::butterworth(rate,cutoff,order,i,hp);result*=(c[0]+c[1]*z+c[2]*z*z)/(1.+c[3]*z+c[4]*z*z);}
 return std::abs(result);
}
int main(){try{
 check(filters::cutOrder(0)==1&&filters::cutOrder(1)==2&&filters::cutOrder(2)==4&&filters::cutOrder(3)==8,"Published cut indices changed");
 check(filters::graphicSlope(0)==12&&filters::graphicSlope(1)==24&&filters::graphicSlope(2)==48&&filters::graphicSlope(3)==96,"Published EQ indices changed");
 for(double rate:{44100.,48000.,96000.})for(int choice:filters::cutMenu)for(bool hp:{false,true}){
  const int n=filters::cutOrder(choice);check(n>=1&&n<=16,"Invalid filter order");
  check(std::abs(response(rate,1000,1000,n,hp)-std::sqrt(.5))<1e-8,"Butterworth cutoff is not -3.01 dB");
  const double wc=std::tan(std::numbers::pi*1000/rate);const auto hz=[&](double ratio){return rate/std::numbers::pi*std::atan(wc*ratio);};
  const double a=response(rate,hz(hp?.25:4),1000,n,hp),b=response(rate,hz(hp?.125:8),1000,n,hp);
  check(std::abs(20*std::log10(a/b)-n*20*std::log10(2.))<.21,"Stopband order differs from advertised slope");
 }
 int previous=0;for(int i:filters::graphicMenu){check(filters::graphicSlope(i)>previous,"Graphic slope menu is not ascending");previous=filters::graphicSlope(i);}
 check(filters::stepGraphic(1,1)==5&&filters::stepGraphic(3,1)==3,"Slope stepping ignores display order or bounds");
 std::cout<<"PASS 6/12/18/24/36/48/60/72/96 dB cuts, odd poles, cutoff/stopband response, stable stored IDs and sorted stepping\n";
}catch(const std::exception& e){std::cerr<<e.what()<<'\n';return 1;}}
