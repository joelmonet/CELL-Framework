#include <cell/ui_components.hpp>
#include <iostream>
#include <set>

using namespace cell;
using namespace cell::ui;
using namespace cell::ui::components;
namespace {
void check(bool value,const char* text){if(!value)throw std::runtime_error(text);}
bool inside(Rect outer,Rect inner){return std::isfinite(inner.x)&&std::isfinite(inner.y)&&std::isfinite(inner.width)&&std::isfinite(inner.height)&&inner.width>=0&&inner.height>=0&&inner.x>=outer.x&&inner.y>=outer.y&&inner.x+inner.width<=outer.x+outer.width+.000001&&inner.y+inner.height<=outer.y+outer.height+.000001;}
void layout(){
 for(double width:{0.,8.,64.,280.,600.,1320.})for(double height:{0.,1.,40.,132.,596.})for(const auto metrics:{Metrics{},Metrics::touch()}){
  const Rect bounds{12,18,width,height};const auto unit=unitGeometry(bounds,metrics);
  for(const auto box:{unit.header,unit.power,unit.collapse,unit.close,unit.title,unit.body})check(inside(bounds,box),"unit geometry escaped its bounds");
  const auto control=controlGeometry(bounds,metrics);for(const auto box:{control.label,control.interaction,control.value})check(inside(bounds,box),"control faces overlap their outside bounds");
 }
 for(double width:{280.,600.,1320.})for(const auto metrics:{Metrics{},Metrics::touch()}){
  const auto grid=controlGrid({0,0,width,0},12,4,metrics);check(grid.columns>=1&&grid.columns<=4,"grid column bound");
  for(unsigned i=0;i<12;++i){const auto cell=grid.cell(i);check(cell.width>0&&cell.x+cell.width<=width+.001,"responsive grid clipped a control");if(i&&i%grid.columns)check(grid.cell(i-1).x+grid.cell(i-1).width<=cell.x,"grid cells overlapped");}
 }
}
void units(){
 const Parameter frequency{1,"f","Frequency","Hz",20,20000,1000,0,1};double value=4321;ValueFormat hz{ValueUnit::hertz,2};
 check(parseValue(frequency," 1.25 kHz ",hz,value)&&value==1250,"frequency unit conversion failed");
 for(auto text:{"1.25 kHz extra","nan Hz","inf","25 dB","+-300 Hz","20001 Hz"}){value=4321;check(!parseValue(frequency,text,hz,value)&&value==4321,"invalid typed value changed the destination");}
 const Parameter seconds{2,"t","Time","s",0,10,1,0,1};check(parseValue(seconds,"25 ms",{ValueUnit::seconds,3},value)&&value==.025,"milliseconds/seconds conversion");
 const Parameter gain{3,"g","Gain","dB",-60,12,0,0,1};check(formatValue(gain,-.001,{ValueUnit::decibels,1})=="0.0 dB","rounded negative zero was displayed");
 ValueFormat silence{ValueUnit::decibels,1};silence.silenceAtMinimum=true;check(parseValue(gain,"−∞ dB",silence,value)&&value==-60,"defined silence text failed");
 auto meaningless=hz;meaningless.infiniteAtMaximum=true;meaningless.silenceAtMinimum=true;value=111;check(!parseValue(frequency,"inf",meaningless,value)&&value==111&&formatValue(frequency,20000,meaningless)!="∞:1","semantic infinity leaked into another physical dimension");
}
std::uint32_t pixel(const Surface& surface,int x,int y){const auto* row=static_cast<const std::byte*>(surface.pixels())+y*surface.stride();return reinterpret_cast<const std::uint32_t*>(row)[x];}
void raster(){
 Surface source;std::vector<std::uint32_t> pixels(32*16,0xffc03020);source.setPixels(pixels,32,16);const auto original=source.pixels();source.resize(32,16);check(source.pixels()==original,"unchanged dimensions recreated the raster cache");
 const auto* alias=static_cast<const std::uint32_t*>(source.pixels());source.setPixels({alias,32*16},16,32);check(source.width()==16&&source.height()==32&&pixel(source,15,31)==0xffc03020,"aliased pixel resize used released memory");
 Surface destination;destination.resize(320,240);destination.render([&](Canvas& canvas){canvas.fill({0,0,320,240},Color(0xff102030));canvas.image(source,{200,10,32,64});parameterFader(canvas,{10,10,100,132},"Level","0 dB",.5);canvas.fill({250,200,10,10},Color(0xffabcdef));});
 check(pixel(destination,205,15)==0xffc03020&&pixel(destination,0,0)==0xff102030&&pixel(destination,255,205)==0xffabcdef,"cached image blit or balanced control clipping failed");
 bool rejected=false;try{source.setPixels(pixels,12,12);}catch(const std::invalid_argument&){rejected=true;}check(rejected&&source.width()==16&&pixel(source,0,0)==0xffc03020,"invalid pixel upload damaged the previous cache");
}
void identities(){
 const Parameter field{101,"gain","Gain","dB",-60,12,0,0,1};const auto geometry=controlGeometry({0,0,160,132});std::set<std::uint64_t> seen;
 for(unsigned face:{0u,1u,2u})for(const auto& info:parameterInfo(field,0,"Gain","0 dB",geometry,true,face)){check(seen.insert(info.identity).second&&info.hasValue&&info.minimum==-60&&info.maximum==12&&info.accessibleName=="Gain","multiple parameter faces shared an editor identity or lost semantics");}
}
}
int main(){try{layout();units();raster();identities();std::cout<<"PASS: Cell layout, typed physical units, cache aliasing and control identities\n";}catch(const std::exception& error){std::cerr<<error.what()<<'\n';return 1;}}
