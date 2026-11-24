#pragma once
#include <cell/ui.hpp>
#include <cell/ui_values.hpp>
#include <numbers>

namespace cell::ui::components {
enum class Presentation {analytical,character};
struct Metrics {
 double gap=12,labelHeight=18,valueHeight=24,headerHeight=40,tileHeight=132;
 double minimumTileWidth=96,minimumPointerTarget=24,touchTarget=44;
 double labelFont=11,valueFont=12;
 static Metrics touch()noexcept{Metrics value;value.gap=16;value.headerHeight=56;value.tileHeight=168;value.labelHeight=22;value.valueHeight=44;value.minimumTileWidth=116;value.minimumPointerTarget=44;value.labelFont=12;value.valueFont=14;return value;}
};
struct Palette {
 Color background{0xff0d171c},panel{0xff15242b},surface{0xff203640},edge{0xff53707e};
 Color ink{0xffedf2f5},muted{0xffaebec7},accent{0xff72d6c3},error{0xffffa684};
};
struct VisualState {bool enabled=true,hovered{},pressed{},focused{},selected{},invalid{};};
struct Grid {
 Rect bounds;unsigned columns=1,count=0;double gap=12,rowHeight=132;
 Rect cell(unsigned index)const noexcept{
  if(index>=count||!columns)return {};
  const double width=std::max(0.,(bounds.width-gap*(columns-1))/columns);
  return {bounds.x+(index%columns)*(width+gap),bounds.y+(index/columns)*(rowHeight+gap),width,rowHeight};
 }
 double height()const noexcept{return count&&columns?((count+columns-1)/columns)*(rowHeight+gap)-gap:0;}
};
inline Grid controlGrid(Rect bounds,unsigned count,unsigned maximumColumns=4,Metrics metrics={})noexcept{
 const auto fitting=unsigned(std::max(1.,std::floor((std::max(0.,bounds.width)+metrics.gap)/(metrics.minimumTileWidth+metrics.gap))));
 return {bounds,std::min(std::max(1u,count),std::min(std::max(1u,maximumColumns),fitting)),count,metrics.gap,metrics.tileHeight};
}
struct UnitGeometry {Rect panel,header,power,title,collapse,close,body;};
inline UnitGeometry unitGeometry(Rect bounds,Metrics metrics={})noexcept{
 const double pad=std::min(8.,std::max(0.,bounds.width)/2),height=std::clamp(metrics.headerHeight,0.,std::max(0.,bounds.height)),button=std::max(0.,std::min({std::max(28.,metrics.minimumPointerTarget),height-8,(bounds.width-2*pad-8)/3})),y=bounds.y+(height-button)/2;
 const Rect header{bounds.x,bounds.y,std::max(0.,bounds.width),height};
 return {bounds,header,{bounds.x+pad,y,button,button},
  {std::min(bounds.x+bounds.width,bounds.x+pad+button+8),bounds.y,std::max(0.,bounds.width-2*pad-3*button-24),height},
  {std::max(bounds.x,bounds.x+bounds.width-pad-2*button-4),y,button,button},{std::max(bounds.x,bounds.x+bounds.width-pad-button),y,button,button},
  {bounds.x+std::min(metrics.gap,std::max(0.,bounds.width)/2),bounds.y+std::min(height+metrics.gap,std::max(0.,bounds.height)),std::max(0.,bounds.width-2*metrics.gap),std::max(0.,bounds.height-height-2*metrics.gap)}};
}
inline void centeredText(Canvas& canvas,Rect box,std::string_view text,double size,Color ink,bool numeric=false){
 canvas.save();canvas.clip(box);
 const double width=numeric?canvas.monoWidth(text,size):canvas.textWidth(text,size,500);
 const Point baseline{box.x+(box.width-width)/2,box.y+(box.height+size*.7)/2};
 if(numeric)canvas.textMono(baseline,text,size,ink);else canvas.textWeighted(baseline,text,size,ink,500);
 canvas.restore();
}
inline void focusRing(Canvas& canvas,Rect bounds,VisualState state,const Palette& palette){if(state.focused)canvas.strokeRounded(bounds.inset(1),5,palette.accent,2);}
inline void button(Canvas& canvas,Rect bounds,std::string_view caption,VisualState state={},Palette palette={},Metrics metrics={}){
 canvas.rounded(bounds,4,state.pressed?palette.background:state.selected?palette.surface:palette.panel);
 canvas.strokeRounded(bounds.inset(.5),4,state.invalid?palette.error:state.hovered?palette.accent:palette.edge);
 centeredText(canvas,bounds,caption,metrics.labelFont,state.enabled?palette.ink:palette.muted);
 if(state.selected)canvas.rounded({bounds.x+6,bounds.y+bounds.height-4,bounds.width-12,2},1,palette.accent);
 focusRing(canvas,bounds,state,palette);
}
struct ControlGeometry {Rect bounds,label,interaction,value;};
inline ControlGeometry controlGeometry(Rect bounds,Metrics metrics={})noexcept{
 const double height=std::max(0.,bounds.height),width=std::max(0.,bounds.width),label=std::clamp(metrics.labelHeight,0.,height),value=std::clamp(metrics.valueHeight,0.,height-label),gap=std::min(6.,std::max(0.,height-label-value)/2),pad=std::min(4.,width/2);
 return {bounds,{bounds.x,bounds.y,width,label},
  {bounds.x,bounds.y+label+gap,width,std::max(0.,height-label-value-2*gap)},
  {bounds.x+pad,bounds.y+height-value,std::max(0.,width-2*pad),value}};
}
inline ControlGeometry parameterKnob(Canvas& canvas,Rect bounds,std::string_view label,std::string_view value,double normalized,VisualState state={},Palette palette={},Metrics metrics={},Presentation presentation=Presentation::analytical){
 canvas.save();canvas.clip(bounds);const auto parts=controlGeometry(bounds,metrics);normalized=std::isfinite(normalized)?std::clamp(normalized,0.,1.):0;
 centeredText(canvas,parts.label,label,metrics.labelFont,state.enabled?palette.ink:palette.muted);
 const Point center{parts.interaction.x+parts.interaction.width/2,parts.interaction.y+parts.interaction.height/2};
 const double radius=std::max(2.,std::min(parts.interaction.width,parts.interaction.height)*.38),start=-std::numbers::pi*.75,angle=start+normalized*std::numbers::pi*1.5;
 std::array<Point,49> arc;
 for(unsigned i=0;i<arc.size();++i){const double a=start+std::numbers::pi*1.5*i/(arc.size()-1);arc[i]={center.x+std::sin(a)*(radius+4),center.y-std::cos(a)*(radius+4)};}
 canvas.polyline(arc,palette.edge,1.5);
 if(normalized>0){const auto count=std::min(arc.size(),std::size_t(std::ceil(normalized*(arc.size()-1)))+1);canvas.polyline({arc.data(),count},state.enabled?palette.accent:palette.muted,2);}
 if(presentation==Presentation::character){
  canvas.rounded({center.x-radius+1,center.y-radius+2,radius*2,radius*2},radius,Color(0x80000000));
  canvas.gradient({center.x-radius,center.y-radius,radius*2,radius*2},palette.surface,palette.background,radius);canvas.strokeCircle(center,radius,palette.edge);
  for(unsigned n=0;n<20;++n){const double a=n*2*std::numbers::pi/20;const Point outer{center.x+std::sin(a)*radius*.94,center.y-std::cos(a)*radius*.94},inner{center.x+std::sin(a)*radius*.8,center.y-std::cos(a)*radius*.8};canvas.line(inner,outer,n<10?Color(0x506d8587):Color(0x700d171b),.7);}
 }
 else{canvas.circle(center,radius,palette.surface);canvas.strokeCircle(center,radius,state.hovered?palette.accent:palette.edge);}
 canvas.line({center.x+std::sin(angle)*radius*.55,center.y-std::cos(angle)*radius*.55},{center.x+std::sin(angle)*radius*.88,center.y-std::cos(angle)*radius*.88},state.enabled?palette.ink:palette.muted,2);
 canvas.rounded(parts.value,3,palette.background);canvas.strokeRounded(parts.value.inset(.5),3,state.invalid?palette.error:palette.edge);
 centeredText(canvas,parts.value,value,metrics.valueFont,state.invalid?palette.error:state.enabled?palette.ink:palette.muted,true);
 focusRing(canvas,bounds,state,palette);canvas.restore();return parts;
}
inline ControlGeometry parameterFader(Canvas& canvas,Rect bounds,std::string_view label,std::string_view value,double normalized,VisualState state={},Palette palette={},Metrics metrics={}){
 canvas.save();canvas.clip(bounds);const auto parts=controlGeometry(bounds,metrics);normalized=std::isfinite(normalized)?std::clamp(normalized,0.,1.):0;
 centeredText(canvas,parts.label,label,metrics.labelFont,state.enabled?palette.ink:palette.muted);
 const double pad=std::min(12.,parts.interaction.width/2),width=std::max(0.,parts.interaction.width-2*pad),y=parts.interaction.y+parts.interaction.height/2;
 canvas.rounded({parts.interaction.x+pad,y-2,width,4},2,palette.background);
 canvas.rounded({parts.interaction.x+pad,y-2,width*normalized,4},2,state.enabled?palette.accent:palette.muted);
 const double x=parts.interaction.x+pad+width*normalized;
 canvas.rounded({x-6,y-12,12,24},3,palette.surface);canvas.strokeRounded({x-6,y-12,12,24},3,state.hovered?palette.accent:palette.edge);
 canvas.line({x,y-6},{x,y+6},state.enabled?palette.ink:palette.muted,2);
 canvas.rounded(parts.value,3,palette.background);canvas.strokeRounded(parts.value.inset(.5),3,state.invalid?palette.error:palette.edge);
 centeredText(canvas,parts.value,value,metrics.valueFont,state.invalid?palette.error:state.enabled?palette.ink:palette.muted,true);focusRing(canvas,bounds,state,palette);canvas.restore();return parts;
}
inline std::array<ControlInfo,2> parameterInfo(const Parameter& field,double plain,std::string_view label,std::string value,const ControlGeometry& geometry,bool enabled=true,std::uint32_t face=0){
 std::array<ControlInfo,2> result;
 for(unsigned i=0;i<2;++i){auto& info=result[i];info.id=field.id;info.identity=(std::uint64_t(field.id)<<32)|(std::uint64_t(face&0x3fffffffu)<<2)|(i+1);info.bounds=i?geometry.value:geometry.interaction;info.role=i?ControlInfo::Role::textbox:ControlInfo::Role::slider;info.label=label;info.accessibleName=label;info.valueText=value;info.unit=field.unit;info.value=plain;info.minimum=field.minimum;info.maximum=field.maximum;info.hasValue=true;info.enabled=enabled;}
 return result;
}
}
