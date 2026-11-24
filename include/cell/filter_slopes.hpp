// SPDX-License-Identifier: Apache-2.0
#pragma once
#include <algorithm>
#include <array>
#include <cmath>
#include <numbers>
namespace cell::filters {
// Values are persisted choice IDs. Append; never reorder an old ID.
inline constexpr std::array cutDb{6,12,24,48,18,36,60,72,96};
inline constexpr std::array graphicDb{12,24,48,96,18,36,60,72};
inline constexpr std::array cutMenu{0,1,4,2,5,3,6,7,8};
inline constexpr std::array graphicMenu{0,4,1,5,2,6,7,3};
inline constexpr int cutOrder(int choice){return cutDb[std::clamp(choice,0,int(cutDb.size()-1))]/6;}
inline constexpr int graphicSlope(int choice){return graphicDb[std::clamp(choice,0,int(graphicDb.size()-1))];}
inline constexpr int graphicSections(int choice){return graphicSlope(choice)/12;}
inline constexpr int stepGraphic(int choice,int steps){int at=0;for(unsigned i=0;i<graphicMenu.size();++i)if(graphicMenu[i]==choice)at=int(i);return graphicMenu[std::clamp(at+steps,0,int(graphicMenu.size()-1))];}
inline constexpr int graphicRank(int choice){for(unsigned i=0;i<graphicMenu.size();++i)if(graphicMenu[i]==choice)return int(i);return 0;}
inline constexpr int sections(int poles){return (poles+1)/2;}
// Normalized direct-form biquad, with a first-order section for odd orders.
inline std::array<double,5> butterworth(double rate,double hz,int poles,int stage,bool hp){
 poles=std::clamp(poles,1,16);const double k=std::tan(std::numbers::pi*std::clamp(hz,5.,rate*.45)/rate);
 if((poles&1)&&stage==0){const double n=1/(1+k);return {hp?n:k*n,hp?-n:k*n,0,(k-1)*n,0};}
 const int pair=stage-(poles&1);const double q=1/(2*std::cos((2*pair+1+(poles&1))*std::numbers::pi/(2*poles)));
 const double n=1/(1+k/q+k*k),b=hp?n:k*k*n;
 return {b,hp?-2*b:2*b,b,2*(k*k-1)*n,(1-k/q+k*k)*n};
}
}
