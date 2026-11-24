// SPDX-License-Identifier: Apache-2.0
#include <cell/scroll_viewport.hpp>
#include <iostream>
#include <limits>
#include <stdexcept>
using namespace cell::ui;
void check(bool value,const char* why){if(!value)throw std::runtime_error(why);}
int main(){try{
 check(revealViewportScroll(50,500,1500,100,300)==50,"Already visible content moved");
 check(revealViewportScroll(0,500,1500,400,700)==200,"Reveal moved farther than needed");
 check(revealViewportScroll(500,500,1500,200,400)==200,"Earlier content was not revealed");
 check(revealViewportScroll(0,500,1800,600,1700)==600,"Wide module's leading controls were scrolled out of view");
 check(clampViewportScroll(1000,500,700)==200,"Shrinking content left an empty overscrolled viewport");
 check(clampViewportScroll(1000,500,400)==0,"Small content retained stale scrolling");
 check(clampViewportScroll(std::numeric_limits<double>::quiet_NaN(),500,400)==0,"Invalid host geometry propagated");
 check(resizeViewportScroll(100,500,1500,650)==150,"Resize failed to reveal the trailing edge");
 check(resizeViewportScroll(100,500,1500,550)==100,"Reverse resize lost its original scroll anchor");
 check(resizeViewportScroll(100,500,400,390)==0,"Resize shrink ignored the content bound");
 std::cout<<"PASS minimal reveal, leading-edge visibility, content shrink, unchanged visible viewport and invalid geometry\n";
}catch(const std::exception& e){std::cerr<<e.what()<<'\n';return 1;}}
