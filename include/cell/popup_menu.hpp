// SPDX-License-Identifier: Apache-2.0
#pragma once
#include <cell/ui.hpp>
#include <cell/ui_defaults.hpp>
#include <chrono>
#include <optional>

namespace cell::ui {
// Owned immediate-mode popup tree. Selection is explicit; navigation and
// hovering never execute a command. All methods belong to the GUI thread.
class PopupMenu {
public:
    struct Item {
        std::string text,label;
        int command{};
        bool enabled=true,checked=false,separator=false;
        std::vector<Item> children;
        std::function<void(Canvas&,Rect)> graphic;
        bool activateParent{};
        // Explicit activation chooses the first available leaf; hover/right still browse.
        bool selectFirstChild{};
    };
    struct Result {bool handled{},changed{},closed{};std::optional<int> command;};
    using Clock=std::chrono::steady_clock;
    void open(std::vector<Item> items,Point anchor){
        advanceGeneration();rows_.clear();controls_.clear();columns_.clear();items_=std::move(items);anchor_=anchor;path_.clear();first_.assign(1,0);cursor_.assign(1,0);depth_=0;pending_.reset();pressed_.reset();open_=true;reveal_=true;
    }
    void close(){if(open_)advanceGeneration();open_=false;rows_.clear();controls_.clear();pending_.reset();pressed_.reset();}
    bool isOpen()const noexcept{return open_;}
    // Capture before starting asynchronous menu work, including before paint.
    // Reopen/close invalidates the token; navigation within a menu retains it.
    std::uint64_t generation()const noexcept{return generation_;}
    bool isCurrent(std::uint64_t token)const noexcept{return open_&&token&&token==generation_;}
    void maximumWidth(double width){maximumWidth_=std::clamp(width,170.,800.);}
    std::span<const ControlInfo> controls()const{return controls_;}
    std::uint64_t focusedControl()const{for(const auto& row:rows_)if(row.depth==depth_&&row.index==cursor_[depth_])return row.identity;return 0;}
    void paint(Canvas& canvas,Rect viewport){
        layout(canvas,viewport);if(!open_)return;
        for(const auto& column:columns_){canvas.rounded({column.x+2,column.y+3,column.width,column.height},3,Color(0x65000000));canvas.rounded(column,3,Color(0xff24343b));canvas.strokeRounded(column.inset(.5),3,Color(0xff657d85));}
        for(const auto& row:rows_){const auto& item=*row.item;
            if(item.separator){canvas.line({row.box.x+8,row.box.y+row.box.height/2},{row.box.x+row.box.width-8,row.box.y+row.box.height/2},Color(0xff52666d));continue;}
            if(row.depth==depth_&&row.index==cursor_[depth_]&&item.enabled)canvas.rounded(row.box,2,Color(0xff356e76));
            const Color ink(item.enabled?0xffedf3f2:0xff87999f);const double graphicWidth=item.graphic?78:0;
            if(item.graphic)item.graphic(canvas,{row.box.x+18,row.box.y+4,70,row.box.height-8});
            canvas.save();canvas.clip({row.box.x+19+graphicWidth,row.box.y,row.box.width-38-graphicWidth,row.box.height});canvas.textWeighted({row.box.x+22+graphicWidth,row.box.y+(row.box.height+9.4)/2},item.text,13,ink,500);canvas.restore();
            if(item.checked){const double x=row.box.x+6,y=row.box.y+row.box.height/2;canvas.line({x,y},{x+3,y+3},ink,1.4);canvas.line({x+3,y+3},{x+9,y-4},ink,1.4);}
            if(!item.children.empty()){const double x=row.box.x+row.box.width-9,y=row.box.y+row.box.height/2;canvas.line({x-2,y-4},{x+2,y},ink,1.3);canvas.line({x+2,y},{x-2,y+4},ink,1.3);}
        }
    }
    Result pointer(const PointerEvent& event){
        if(!open_)return {};const auto hit=at(event.position);
        if(event.action==PointerAction::cancel||event.action==PointerAction::context){close();return {true,true,true,{}};}
        if(event.action==PointerAction::wheel){if(hit){auto& first=first_[hit->depth];first=std::max(0,first-int(std::clamp(event.wheel,-50.,50.)*3));reveal_=false;}return {true,true};}
        if(event.action==PointerAction::down){if(!hit){close();return {true,true,true,{}};}pressed_=event.identity;select(*hit);if(!hit->item->children.empty()&&(!hit->item->activateParent||event.position.x>hit->box.x+hit->box.width-25))expand(*hit);return {true,true};}
        if(event.action==PointerAction::move){if(hit&&hit->item->enabled&&!hit->item->separator){select(*hit);if(hit->item->activateParent&&event.position.x<=hit->box.x+hit->box.width-25){pending_.reset();if(path_.size()>hit->depth)path_.resize(hit->depth);return {true,true};}if(!hit->item->children.empty()){if(!pending_||pending_->identity!=hit->identity){pending_=*hit;pendingAt_=Clock::now();}}else if(path_.size()>hit->depth){if(!pending_||pending_->identity!=hit->identity){pending_=*hit;pendingAt_=Clock::now();}}else pending_.reset();}return {true,true};}
        if(event.action==PointerAction::up){const bool owned=pressed_&&*pressed_==event.identity;pressed_.reset();if(owned&&hit){if((hit->item->activateParent||hit->item->selectFirstChild)&&!hit->item->children.empty()&&event.position.x>hit->box.x+hit->box.width-25){expand(*hit);return {true,true};}return choose(*hit);}return {true};}
        return {true};
    }
    bool tick(Clock::time_point now=Clock::now()){
        if(open_&&pending_&&now-pendingAt_>=InteractionDefaults::submenuHover){const auto row=*pending_;pending_.reset();if(row.item->children.empty())path_.resize(row.depth);else expand(row);return true;}return false;
    }
    Result key(int key){
        if(!open_||key==32)return {};
        if(key==27||key==9||key==keys::reverseTab){close();return {true,true,true,{}};}
        const auto* list=listAt(depth_);if(!list||list->empty())return {true};pending_.reset();
        if(key==keys::left){if(depth_){--depth_;path_.resize(depth_);}return {true,true};}
        int index=std::clamp(cursor_[depth_],0,int(list->size())-1);
        if(key==13||key==keys::right){const auto& item=(*list)[unsigned(index)];Row row{{},depth_,index,&item,identity(depth_,index)};if(!item.children.empty()&&(key==keys::right||(!item.activateParent&&!item.selectFirstChild))){expand(row);++depth_;ensure(depth_);cursor_[depth_]=firstSelectable(*listAt(depth_));reveal_=true;return {true,true};}return key==13?choose(row):Result{true};}
        int direction=0;if(key==keys::down)direction=1;else if(key==keys::up)direction=-1;else if(key==keys::home)index=-1,direction=1;else if(key==keys::end)index=int(list->size()),direction=-1;
        if(direction){for(unsigned n=0;n<list->size();++n){index=(index+direction+int(list->size()))%int(list->size());if((*list)[unsigned(index)].enabled&&!(*list)[unsigned(index)].separator)break;}cursor_[depth_]=index;reveal_=true;return {true,true};}
        if(key>=33&&key<=126){for(unsigned n=1;n<=list->size();++n){const int next=(index+int(n))%int(list->size());const auto& item=(*list)[unsigned(next)];if(item.enabled&&!item.separator&&!item.text.empty()&&capital(item.text.front())==capital(char(key))){cursor_[depth_]=next;reveal_=true;break;}}}return {true,true};
    }
    Result accessible(std::uint64_t id,AccessibleAction action){for(const auto& row:rows_)if(row.identity==id){select(row);return action==AccessibleAction::activate?choose(row):Result{true,true};}return {};}
private:
    std::uint64_t generation_{};
    void advanceGeneration()noexcept{if(++generation_==0)++generation_;}
    struct Row {Rect box;unsigned depth{};int index{};const Item* item{};std::uint64_t identity{};};
    void ensure(unsigned depth){if(first_.size()<=depth)first_.resize(depth+1);if(cursor_.size()<=depth)cursor_.resize(depth+1);}
    const std::vector<Item>* listAt(unsigned depth)const{const auto* list=&items_;for(unsigned n=0;n<depth;++n){if(n>=path_.size()||path_[n]<0||unsigned(path_[n])>=list->size())return nullptr;list=&(*list)[unsigned(path_[n])].children;}return list;}
    static char capital(char key){return key>='a'&&key<='z'?char(key-'a'+'A'):key;}
    static int firstSelectable(const std::vector<Item>& items){for(unsigned n=0;n<items.size();++n)if(items[n].enabled&&!items[n].separator)return int(n);return 0;}
    std::uint64_t identity(unsigned depth,int index)const{std::uint64_t id=0x504f505550000000ull;for(unsigned n=0;n<depth&&n<path_.size();++n)id=(id^unsigned(path_[n]+1))*1099511628211ull;return (id^unsigned(index+1))*1099511628211ull;}
    std::optional<Row> at(Point p)const{for(auto row=rows_.rbegin();row!=rows_.rend();++row)if(row->box.contains(p))return *row;return {};}
    void select(const Row& row){depth_=row.depth;ensure(depth_);cursor_[depth_]=row.index;}
    void expand(const Row& row){if(!row.item->enabled||row.item->children.empty())return;path_.resize(row.depth+1);path_[row.depth]=row.index;ensure(row.depth+1);first_[row.depth+1]=0;cursor_[row.depth+1]=firstSelectable(row.item->children);}
    static const Item* firstLeaf(const std::vector<Item>& items){for(const auto& item:items)if(item.enabled&&!item.separator){if(item.children.empty())return &item;if(const auto* leaf=firstLeaf(item.children))return leaf;}return nullptr;}
    Result choose(const Row& row){if(!row.item->enabled||row.item->separator)return {true};if(!row.item->children.empty()&&row.item->selectFirstChild){const auto* leaf=firstLeaf(row.item->children);if(!leaf)return {true};const auto command=leaf->command;close();return {true,true,true,command};}if(!row.item->children.empty()&&!row.item->activateParent){expand(row);return {true,true};}const auto command=row.item->command;close();return {true,true,true,command};}
    void layout(Canvas& canvas,Rect viewport){
        rows_.clear();columns_.clear();controls_.clear();if(!open_)return;
        if(!std::isfinite(viewport.width)||!std::isfinite(viewport.height)||viewport.width<100||viewport.height<50)return;
        Point origin{std::clamp(anchor_.x,viewport.x+6,viewport.x+viewport.width-86),std::clamp(anchor_.y,viewport.y+6,viewport.y+viewport.height-40)};
        for(unsigned depth=0;depth<=path_.size();++depth){const auto* list=listAt(depth);if(!list||list->empty())break;ensure(depth);
            double width=170,total=12;for(const auto& item:*list){width=std::max(width,canvas.textWidth(item.text,13,500)+52+(item.graphic?78:0));total+=item.separator?8:item.graphic?38:27;}
            width=std::min({width,maximumWidth_,viewport.width-12});double height=std::min(total,viewport.height-12);
            if(!depth){const double below=viewport.y+viewport.height-6-origin.y,above=origin.y-viewport.y-6;
                if(total<=below||(total>above&&(below>=96||below>=above)))height=std::min(height,below);
                else{height=std::min(height,above);origin.y-=height;}
            }
            if(depth){const auto& previous=columns_.back();const bool left=depth>1&&previous.x<columns_[depth-2].x;origin.x=left?previous.x-width+1:previous.x+previous.width-1;if(origin.x+width>viewport.x+viewport.width-6)origin.x=previous.x-width+1;else if(origin.x<viewport.x+6)origin.x=previous.x+previous.width-1;for(const auto& row:rows_)if(row.depth+1==depth&&row.index==path_[depth-1]){origin.y=row.box.y-6;break;}}
            Rect column{std::clamp(origin.x,viewport.x+6,viewport.x+viewport.width-width-6),std::clamp(origin.y,viewport.y+6,viewport.y+viewport.height-height-6),width,height};columns_.push_back(column);
            const int maximum=int(list->size())-1;first_[depth]=std::clamp(first_[depth],0,maximum);
            if(reveal_&&depth==depth_){if(cursor_[depth]<first_[depth])first_[depth]=cursor_[depth];double occupied=0;for(int i=first_[depth];i<=cursor_[depth]&&i<=maximum;++i)occupied+=(*list)[unsigned(i)].separator?8:(*list)[unsigned(i)].graphic?38:27;while(occupied>height-12&&first_[depth]<cursor_[depth]){const auto& item=(*list)[unsigned(first_[depth]++)];occupied-=item.separator?8:item.graphic?38:27;}}
            double y=column.y+6;for(int n=first_[depth];n<=maximum;++n){const auto& item=(*list)[unsigned(n)];const double rowHeight=item.separator?8:item.graphic?38:27;if(y+rowHeight>column.y+height-6)break;Row row{{column.x+6,y,width-12,rowHeight},depth,n,&item,identity(depth,n)};rows_.push_back(row);ControlInfo info;info.identity=row.identity;info.bounds=row.box;info.role=ControlInfo::Role::button;info.label=info.accessibleName=item.label.empty()?item.text:item.label;info.enabled=item.enabled&&!item.separator;info.hasValue=item.checked;info.value=item.checked?1:0;controls_.push_back(std::move(info));y+=rowHeight;}
        }reveal_=false;
    }
    std::vector<Item> items_;std::vector<int> path_,first_,cursor_;std::vector<Row> rows_;std::vector<Rect> columns_;std::vector<ControlInfo> controls_;
    Point anchor_;double maximumWidth_=420;unsigned depth_{};bool open_{},reveal_{};std::optional<Row> pending_;Clock::time_point pendingAt_;std::optional<PointerIdentity> pressed_;
};
}
