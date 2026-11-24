// Original Cell Framework implementation; see LICENSE.
#include <cell/timeline.hpp>
#include <array>
#include <cmath>
#include <cstdio>
#include <limits>
#include <stdexcept>

namespace cell::ui {
namespace {
constexpr Color background{0xff14191a},panel{0xff1c2223},line{0xff354041},text{0xffdfe6e4},muted{0xff8c9998},accent{0xffa8d989};
bool finiteRange(double a,double b){return std::isfinite(a)&&std::isfinite(b)&&b>a;}
double clampFinite(double x,double lo,double hi,double fallback){return std::isfinite(x)?std::clamp(x,lo,hi):fallback;}
}
struct TimelineOverview::Impl {
    TimelineOverview& owner;
    std::shared_ptr<const TimelineScene> scene=std::make_shared<const TimelineScene>();
    TimelineViewport view;
    std::optional<std::pair<double,double>> selection,loop;
    double cursor{},scale=1.;
    bool edited{};
    Rect bounds{0,0,1200,600},plot{140,76,1048,512};
    Surface base;
    std::uint64_t revision=1,painted{},baseRevision=1,built{};
    TimelineFrameStats stats;
    enum class Gesture {none,pan,select,left,right} gesture=Gesture::none;
    Point down{},drag{};
    TimelineViewport downView;
    explicit Impl(TimelineOverview& e):owner(e){}
    double span()const{return std::max(1.e-9,scene->end-scene->begin);}
    double rowCount()const{return double(scene->tracks.size());}
    double x(double time)const{return plot.x+(time-scene->begin)/span()*plot.width;}
    double time(double px)const{return scene->begin+std::clamp((px-plot.x)/std::max(1.,plot.width),0.,1.)*span();}
    double row(double py)const{return std::clamp((py-plot.y)/std::max(1.,plot.height)*std::max(1.,rowCount()),0.,std::max(0.,rowCount()-1.));}
    double physical(double value)const{return std::round(value*scale)/scale;}
    Rect viewRect()const {
        const auto rows=std::max(1.,rowCount());
        return {x(view.begin),plot.y+view.firstTrack/rows*plot.height,
                std::max(1./scale,(view.end-view.begin)/span()*plot.width),
                std::max(1./scale,std::min(view.trackCount,rows)/rows*plot.height)};
    }
    TimelineViewport valid(TimelineViewport v)const {
        const auto minimum=std::min(span(),1./192000.);
        const auto width=std::clamp(finiteRange(v.begin,v.end)?v.end-v.begin:span(),minimum,span());
        v.begin=clampFinite(v.begin,scene->begin,scene->end-width,scene->begin);v.end=v.begin+width;
        v.trackCount=clampFinite(v.trackCount,1.,std::max(1.,rowCount()),std::max(1.,rowCount()));
        v.firstTrack=clampFinite(v.firstTrack,0.,std::max(0.,rowCount()-v.trackCount),0.);
        return v;
    }
    void changed(bool waveform=false){++revision;if(waveform)++baseRevision;if(owner.invalidate)owner.invalidate();}
    void emit(TimelineViewport v,std::optional<double> seek={}) {
        view=valid(v);changed();if(owner.navigate)owner.navigate({view,seek});
    }
    Rect button(unsigned index)const {
        if(index==0)return {bounds.width-222.,10,64,28};
        if(index==1)return {bounds.width-152.,10,64,28};
        return {bounds.width-(index==2?82.:44.),10,32,28};
    }
    void build(Canvas& c) {
        c.fill(bounds,background);c.fill({0,0,bounds.width,44},panel);
        c.text({16,27},"Timeline",16,text);
        char count[80];std::snprintf(count,sizeof(count),"%zu tracks",scene->tracks.size());if(bounds.width>=600)c.text({110,26},count,12,muted);
        const std::array<std::string_view,2> labels={"Source","Edited"};
        for(unsigned i=0;i<2;++i){auto r=button(i);
            c.rounded(r,4,Color{(edited==bool(i))?0xff3b5034u:0xff252d2eu});
            c.fittedText(r,labels[i],text,400);}
        auto fit=button(2);c.rounded(fit,4,Color{0xff252d2e});
        c.line({fit.x+8,fit.y+8},{fit.x+24,fit.y+8},text);c.line({fit.x+8,fit.y+20},{fit.x+24,fit.y+20},text);
        c.line({fit.x+8,fit.y+8},{fit.x+8,fit.y+20},text);c.line({fit.x+24,fit.y+8},{fit.x+24,fit.y+20},text);
        auto done=button(3);c.line({done.x+10,done.y+9},{done.x+22,done.y+21},text,1.5);c.line({done.x+22,done.y+9},{done.x+10,done.y+21},text,1.5);
        c.fill(plot,Color{0xff111617});
        c.save();c.clip(plot);
        for(const auto& section:scene->sections){if(!finiteRange(section.begin,section.end))continue;
            const auto a=std::max(plot.x,x(section.begin)),b=std::min(plot.x+plot.width,x(section.end));
            if(b>a){c.fill({a,plot.y,b-a,2./scale},Color{section.color});}}
        const auto rows=std::max(1.,rowCount());
        for(std::size_t i=0;i<scene->tracks.size();++i){
            const auto top=physical(plot.y+double(i)/rows*plot.height),bottom=physical(plot.y+double(i+1)/rows*plot.height);
            if(bottom<=top)continue;
            const auto& track=scene->tracks[i];
            c.save();c.clip({plot.x,top,plot.width,bottom-top});
            if(i%2)c.fill({plot.x,top,plot.width,bottom-top},Color{0xff171d1e});
            const double middle=(top+bottom)*.5,amplitude=std::max(.5/scale,(bottom-top)*.38);
            for(const auto& clip:track.clips){
                if(!clip.peaks||clip.peaks->empty()||!std::isfinite(clip.begin)||!std::isfinite(clip.duration)||clip.duration<=0)continue;
                const auto a=std::max(plot.x,x(clip.begin)),b=std::min(plot.x+plot.width,x(clip.begin+clip.duration));
                if(b<=a)continue;
                const int begin=int(std::floor(a*scale)),end=int(std::ceil(b*scale));
                for(int px=begin;px<end;++px){
                    const auto t0=time(double(px)/scale),t1=time(double(px+1)/scale);
                    const auto first=std::size_t(std::clamp((t0-clip.begin)/clip.duration,0.,1.)*double(clip.peaks->size()-1));
                    const auto last=std::min(clip.peaks->size(),std::max(first+1,std::size_t(std::ceil(std::clamp((t1-clip.begin)/clip.duration,0.,1.)*double(clip.peaks->size())))));
                    float lo=0,hi=0;for(auto n=first;n<last;++n){const auto& p=(*clip.peaks)[n];if(std::isfinite(p.low))lo=std::min(lo,p.low);if(std::isfinite(p.high))hi=std::max(hi,p.high);}
                    const auto y1=std::clamp(physical(middle-std::clamp(double(hi),0.,1.)*amplitude),top,bottom-1./scale);
                    const auto y2=std::clamp(physical(middle-std::clamp(double(lo),-1.,0.)*amplitude),top,bottom-1./scale);
                    c.fill({double(px)/scale,y1,1./scale,std::max(1./scale,y2-y1)},Color{track.color});
                }
            }
            c.restore();
        }
        c.restore();
        for(std::size_t i=0;i<scene->tracks.size();++i){const auto top=physical(plot.y+double(i)/rows*plot.height),bottom=physical(plot.y+double(i+1)/rows*plot.height);
            if(bottom-top>=14.)c.fittedText({12,top,plot.x-22,bottom-top},scene->tracks[i].name,Color{scene->tracks[i].color});}
        for(unsigned i=0;i<=8;++i){const auto at=scene->begin+span()*double(i)/8.;const auto px=physical(x(at));
            char label[40];std::snprintf(label,sizeof(label),"%.1f",at);c.textMono({std::min(px,plot.x+plot.width-42),plot.y-12},label,10,muted);
            c.line({px,plot.y},{px,plot.y+plot.height},line,1./scale);}
    }
};
TimelineOverview::TimelineOverview():impl_(std::make_unique<Impl>(*this)){}
TimelineOverview::~TimelineOverview()=default;
void TimelineOverview::setScene(std::shared_ptr<const TimelineScene> s){if(!s||s==impl_->scene)return;if(!finiteRange(s->begin,s->end))throw std::invalid_argument("Invalid timeline scene range");impl_->scene=std::move(s);impl_->view=impl_->valid(impl_->view);impl_->changed(true);}
void TimelineOverview::setViewport(TimelineViewport v){v=impl_->valid(v);const auto old=impl_->view;if(v.begin==old.begin&&v.end==old.end&&v.firstTrack==old.firstTrack&&v.trackCount==old.trackCount)return;impl_->view=v;impl_->changed();}
TimelineViewport TimelineOverview::viewport()const noexcept{return impl_->view;}
void TimelineOverview::setCursor(double v){if(std::isfinite(v)&&v!=impl_->cursor){impl_->cursor=v;impl_->changed();}}
void TimelineOverview::setSelection(std::optional<std::pair<double,double>> v){if(v!=impl_->selection){impl_->selection=v;impl_->changed();}}
void TimelineOverview::setLoop(std::optional<std::pair<double,double>> v){if(v!=impl_->loop){impl_->loop=v;impl_->changed();}}
void TimelineOverview::setBackingScale(double v){if(!std::isfinite(v)||v<=0||v>16.)throw std::invalid_argument("Invalid timeline backing scale");if(v!=impl_->scale){impl_->scale=v;impl_->changed(true);}}
void TimelineOverview::setEdited(bool v){if(v!=impl_->edited){impl_->edited=v;impl_->changed(true);}}
bool TimelineOverview::edited()const noexcept{return impl_->edited;}
bool TimelineOverview::cancelGesture(){if(impl_->gesture==Impl::Gesture::none)return false;impl_->gesture=Impl::Gesture::none;impl_->changed();return true;}
Rect TimelineOverview::plotBounds()const noexcept{return impl_->plot;}
Rect TimelineOverview::viewportBounds()const noexcept{return impl_->viewRect();}
TimelineFrameStats TimelineOverview::stats()const noexcept{auto s=impl_->stats;s.tracks=impl_->scene->tracks.size();return s;}
void TimelineOverview::paint(Canvas& c,Rect bounds){auto& p=*impl_;if(bounds.width<1||bounds.height<1)return;
    if(bounds.width!=p.bounds.width||bounds.height!=p.bounds.height){p.bounds=bounds;p.plot={std::min(140.,bounds.width*.2),76,std::max(1.,bounds.width-std::min(140.,bounds.width*.2)-12),std::max(1.,bounds.height-88)};++p.baseRevision;}
    const double pw=std::ceil(bounds.width*p.scale),ph=std::ceil(bounds.height*p.scale);
    if(pw>double(std::numeric_limits<int>::max())||ph>double(std::numeric_limits<int>::max()))throw std::length_error("Timeline surface size exceeds native coordinates");
    if(p.base.width()!=int(pw)||p.base.height()!=int(ph)){p.base.resize(int(pw),int(ph));++p.baseRevision;}
    if(p.built!=p.baseRevision){p.base.render([&](Canvas& base){base.scale(p.scale,p.scale);p.build(base);});p.built=p.baseRevision;++p.stats.baseBuilds;}
    c.image(p.base,bounds);c.save();c.clip(p.plot);
    const auto band=[&](const auto& range,Color color){if(range&&finiteRange(range->first,range->second)){const auto a=p.x(range->first),b=p.x(range->second);c.fill({a,p.plot.y,b-a,p.plot.height},color);}};
    band(p.loop,Color{0x163f93a8});band(p.selection,Color{0x224f7242});
    auto view=p.viewRect();c.fill(view,Color{0x183fc976});c.strokeRounded(view,0,accent,1.5/p.scale);
    if(p.cursor>=p.scene->begin&&p.cursor<=p.scene->end){const auto px=p.physical(p.x(p.cursor));c.line({px,p.plot.y},{px,p.plot.y+p.plot.height},Color{0xffffbf70},1./p.scale);}
    if(p.gesture==Impl::Gesture::select){const auto a=std::min(p.down.x,p.drag.x),b=std::max(p.down.x,p.drag.x);c.fill({a,p.plot.y,b-a,p.plot.height},Color{0x3355a3ae});}
    c.restore();++p.stats.paints;p.painted=p.revision;
}
bool TimelineOverview::pointer(const PointerEvent& e){auto& p=*impl_;const auto point=e.position;
    if(e.action==PointerAction::cancel)return cancelGesture();
    if(e.action==PointerAction::context){if(contextMenu)contextMenu(point);return true;}
    if(e.action==PointerAction::down){
        for(unsigned i=0;i<4;++i){auto button=p.button(i);if(button.contains(point)){
            if(i<2){setEdited(bool(i));if(editedChanged)editedChanged(bool(i));}
            else if(i==2)p.emit({p.scene->begin,p.scene->end,0.,std::max(1.,p.rowCount())});else if(close)close();return true;}}
        if(!p.plot.contains(point))return false;
        if(e.reset){auto v=p.view;const auto width=v.end-v.begin;v.begin=p.time(point.x)-width*.5;v.end=v.begin+width;p.emit(v,p.time(point.x));return true;}
        p.down=p.drag=point;p.downView=p.view;const auto view=p.viewRect();
        if(point.y>=view.y&&point.y<=view.y+view.height&&std::abs(point.x-view.x)<6.)p.gesture=Impl::Gesture::left;
        else if(point.y>=view.y&&point.y<=view.y+view.height&&std::abs(point.x-view.x-view.width)<6.)p.gesture=Impl::Gesture::right;
        else p.gesture=view.contains(point)?Impl::Gesture::pan:Impl::Gesture::select;p.changed();return true;
    }
    if(e.action==PointerAction::move&&p.gesture!=Impl::Gesture::none){p.drag=point;auto v=p.downView;
        const auto shift=(point.x-p.down.x)/std::max(1.,p.plot.width)*p.span();
        if(p.gesture==Impl::Gesture::pan){v.begin+=shift;v.end+=shift;v.firstTrack+=(point.y-p.down.y)/std::max(1.,p.plot.height)*std::max(1.,p.rowCount());p.emit(v);}
        else if(p.gesture==Impl::Gesture::left){v.begin=std::min(v.end-1./192000.,p.time(point.x));p.emit(v);}
        else if(p.gesture==Impl::Gesture::right){v.end=std::max(v.begin+1./192000.,p.time(point.x));p.emit(v);}
        else p.changed();return true;}
    if(e.action==PointerAction::up&&p.gesture!=Impl::Gesture::none){
        if(p.gesture==Impl::Gesture::select){auto v=p.downView;if(std::abs(point.x-p.down.x)>3.){v.begin=p.time(std::min(point.x,p.down.x));v.end=p.time(std::max(point.x,p.down.x));}
            else{const auto width=v.end-v.begin;v.begin=p.time(point.x)-width*.5;v.end=v.begin+width;v.firstTrack=p.row(point.y);}p.emit(v);}
        p.gesture=Impl::Gesture::none;p.changed();return true;}
    if(e.action==PointerAction::wheel&&p.plot.contains(point)){auto v=p.view;
        if(e.fine){const auto amount=e.wheel*(v.end-v.begin)*.1;v.begin+=amount;v.end+=amount;}
        else v.firstTrack-=e.wheel*std::max(1.,v.trackCount/8.);p.emit(v);return true;}
    return false;
}
bool TimelineOverview::keyboard(const KeyEvent& e){if(e.control||e.alt||e.command)return false;auto& p=*impl_;
    if(e.key=='t'||e.key=='T'){if(close)close();return true;}
    if(e.key=='f'||e.key=='F'){p.emit({p.scene->begin,p.scene->end,0.,std::max(1.,p.rowCount())});return true;}
    if(e.key==27)return cancelGesture();return false;
}
bool TimelineOverview::tick(){return impl_->painted!=impl_->revision;}
void TimelineOverview::cancelInput(){cancelGesture();}
}
