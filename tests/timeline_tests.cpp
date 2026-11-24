// Original Cell Framework implementation; see LICENSE.
#include <cell/timeline.hpp>
#include <chrono>
#include <cmath>
#include <cstring>
#include <iostream>
#include <limits>
#include <stdexcept>

namespace {
void check(bool v,const char* m){if(!v)throw std::runtime_error(m);}
std::uint64_t hash(const cell::ui::Surface& surface){
    auto* bytes=static_cast<const unsigned char*>(surface.pixels());std::uint64_t h=1469598103934665603ull;
    for(int row=0;row<surface.height();++row)for(int col=0;col<surface.width()*4;++col){h^=bytes[row*surface.stride()+col];h*=1099511628211ull;}return h;
}
}
int main(int argc,char** argv){using namespace cell::ui;try{
    auto scene=std::make_shared<TimelineScene>();scene->end=4.*60.*60.;
    auto peaks=std::make_shared<std::vector<TimelinePeak>>();for(int i=0;i<512;++i){float x=float(.1+.8*std::abs(std::sin(i*.17)));peaks->push_back({-x,x});}
    for(unsigned i=0;i<300;++i){TimelineTrack track;track.id=std::to_string(i);track.name="Track "+std::to_string(i+1);track.color=i<25?0xffa8d989:i<55?0xffb49be6:0xffe18d8d;track.clips.push_back({double(i%7)*200.,7000.,peaks});track.clips.push_back({8500.+i%5*90.,4000.,peaks});scene->tracks.push_back(std::move(track));}
    TimelineOverview editor;editor.setScene(scene);editor.setViewport({2000.,3000.,0.,25.});
    Surface surface;surface.resize(1200,600);const auto paint=[&]{surface.render([&](Canvas& c){editor.paint(c,{0,0,1200,600});});};
    paint();check(editor.stats().tracks==300&&editor.stats().baseBuilds==1,"Native overview inferred nonexistent tracks or missed its first frame");
    const auto first=hash(surface);for(int i=0;i<30;++i){check(!editor.tick(),"Idle overview remained dirty");paint();check(hash(surface)==first,"Unchanged native overview pixels flickered");}
    for(int i=0;i<100;++i){editor.setCursor(double(i)*15.);paint();}
    check(editor.stats().baseBuilds==1,"Cursor updates rebuilt waveform geometry");
    int navigations=0;std::optional<double> seek;editor.navigate=[&](const TimelineNavigation& event){++navigations;seek=event.seek;};
    auto r=editor.viewportBounds();Point middle{r.x+r.width*.5,r.y+r.height*.5};
    editor.pointer({PointerAction::down,middle});editor.pointer({PointerAction::move,{middle.x+30,middle.y}});editor.pointer({PointerAction::up,{middle.x+30,middle.y}});
    check(navigations&&editor.viewport().begin>2000.,"Dragging the viewport did not pan right");
    const auto before=editor.viewport().begin;editor.pointer({PointerAction::wheel,middle,1.,true});check(editor.viewport().begin>before,"Shift-wheel up did not move right");
    editor.pointer({PointerAction::wheel,middle,-1.,true});check(std::abs(editor.viewport().begin-before)<1.e-8,"Shift-wheel down did not reverse the pan");
    const auto plot=editor.plotBounds();editor.pointer({PointerAction::down,{plot.x+plot.width*.8,plot.y+plot.height*.8},0,false,true});
    check(seek.has_value()&&*seek>scene->end*.7,"Double-click did not request seek at the clicked position");
    editor.pointer({PointerAction::down,{plot.x+plot.width*.4,plot.y+plot.height*.5}});
    editor.pointer({PointerAction::move,{plot.x+plot.width*.6,plot.y+plot.height*.5}});
    editor.pointer({PointerAction::up,{plot.x+plot.width*.6,plot.y+plot.height*.5}});
    check(std::abs((editor.viewport().end-editor.viewport().begin)-scene->end*.2)<1.e-6,"Marquee did not select the requested passage");
    for(double scale:{.75,1.,1.25,1.5,2.}){editor.setBackingScale(scale);paint();const auto baseline=hash(surface);paint();check(hash(surface)==baseline,"Fractional-scale rows or seams changed between paints");}
    bool rejected=false;try{editor.setBackingScale(std::numeric_limits<double>::quiet_NaN());}catch(const std::invalid_argument&){rejected=true;}check(rejected,"Invalid scale reached the renderer");
    editor.keyboard({'f'});check(editor.viewport().begin==0.&&editor.viewport().end==scene->end,"Fit shortcut did not show the whole performance");
    if(argc>1)check(surface.writePng(argv[1]),"Native timeline preview could not be saved");
    std::cout<<"PASS native Cell timeline: 300 actual tracks/4-hour context, stable idle and fractional-DPI pixels, cached cursor, pan/seek/marquee/fit and Shift-wheel navigation\n";
}catch(const std::exception& e){std::cerr<<e.what()<<'\n';return 1;}}
