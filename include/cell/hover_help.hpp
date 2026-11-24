// SPDX-License-Identifier: Apache-2.0
#pragma once
#include <cell/ui.hpp>
#include <algorithm>
#include <chrono>
#include <utility>
#include <string>
#include <vector>

namespace cell::ui {
// A GUI-thread tooltip for immediate-mode editors. Stable control identity
// prevents repaint from restarting the delay or changing a captured gesture.
class HoverHelp {
public:
    using Clock=std::chrono::steady_clock;
    explicit HoverHelp(std::chrono::milliseconds delay=std::chrono::milliseconds(600)):delay_(delay){}
    void observe(Point point,std::uint64_t target,std::string text,Clock::time_point now=Clock::now()){
        if(target!=target_||text!=text_||std::hypot(point.x-at_.x,point.y-at_.y)>2){visible_=false;since_=now;}
        at_=point;target_=target;text_=std::move(text);
    }
    void hide(){target_=0;text_.clear();visible_=false;}
    bool tick(Clock::time_point now=Clock::now()){
        const bool next=target_&&!text_.empty()&&now-since_>=delay_;
        return std::exchange(visible_,next)!=next;
    }
    bool visible()const noexcept{return visible_;}
    std::string_view text()const noexcept{return text_;}
    Rect paint(Canvas& canvas,Rect viewport)const{
        if(!visible_||viewport.width<80||viewport.height<40)return {};
        constexpr double font=11,lineHeight=16,pad=8;
        const double width=std::min(360.,viewport.width-12),limit=width-pad*2;
        std::vector<std::string> lines;std::string line;
        for(std::size_t start=0;start<text_.size();){auto end=text_.find(' ',start);if(end==text_.npos)end=text_.size();auto word=text_.substr(start,end-start);const auto joined=line.empty()?word:line+" "+word;
            if(!line.empty()&&canvas.textWidth(joined,font)>limit){lines.push_back(std::move(line));line=std::move(word);}else line=joined;start=end+1;}
        if(!line.empty())lines.push_back(std::move(line));
        const auto maxLines=std::max(1u,unsigned((viewport.height-12-pad*2)/lineHeight));if(lines.size()>maxLines)lines.resize(maxLines);
        double measured=0;for(const auto& item:lines)measured=std::max(measured,canvas.textWidth(item,font));
        Rect box{at_.x+12,at_.y+20,std::min(width,measured+pad*2),lineHeight*lines.size()+pad*2};
        box.x=std::clamp(box.x,viewport.x+6,viewport.x+viewport.width-box.width-6);
        if(box.y+box.height>viewport.y+viewport.height-6)box.y=at_.y-box.height-12;
        box.y=std::clamp(box.y,viewport.y+6,viewport.y+viewport.height-box.height-6);
        canvas.save();canvas.clip(viewport);canvas.rounded(box,3,Color(0xff151d21));canvas.strokeRounded(box.inset(.5),3,Color(0xffd8c58a));canvas.clip(box.inset(2));
        for(unsigned n=0;n<lines.size();++n)canvas.text({box.x+pad,box.y+pad+font+n*lineHeight},lines[n],font,Color(0xfff0ecda));canvas.restore();return box;
    }
private:
    std::chrono::milliseconds delay_;Clock::time_point since_{};Point at_{};std::uint64_t target_{};std::string text_;bool visible_{};
};
}
