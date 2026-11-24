#include "internal.hpp"
#include <cell/json.hpp>
#include <algorithm>
#include <stdexcept>
namespace cell::web {
namespace detail {
void State::push(Event::Kind kind,std::string text){
    if(closed)return;
    if(text.size()>maxMessageBytes){kind=Event::Kind::error;text="WebView event exceeded 64 KiB";}
    if(kind==Event::Kind::message&&!validJson(text)){kind=Event::Kind::error;text="WebView bridge rejected malformed JSON";}
    if(kind==Event::Kind::error)error=text;
    if(events.size()>=128){events.pop_front();if(kind==Event::Kind::message){kind=Event::Kind::error;text="WebView event queue overflow";}}
    events.push_back({kind,std::move(text)});
}
void validateBounds(Bounds b){
    if(b.width<1||b.height<1||b.width>8192||b.height>8192||b.x<-1048576||b.x>1048576||b.y<-1048576||b.y>1048576)throw std::invalid_argument("WebView physical bounds");
}
bool validJson(std::string_view text) noexcept {try{if(text.empty()||text.size()>maxMessageBytes)return false;(void)json::read(text,json::Limits{maxMessageBytes,8192,maxMessageBytes,32});return true;}catch(...){return false;}}
bool navigationAllowed(std::string_view uri,const Options& options) noexcept {
    if(uri=="about:blank"||uri.starts_with("about:blank#"))return true;
    return options.allowRemoteContent&&(uri.starts_with("https://")||uri.starts_with("http://"));
}
}
struct View::Impl {
    std::thread::id owner=std::this_thread::get_id();
    std::shared_ptr<detail::State> state=std::make_shared<detail::State>();
    std::unique_ptr<detail::NativeView> native;
    void thread() const{if(owner!=std::this_thread::get_id())throw std::logic_error("WebView belongs to its GUI thread");}
};
View::View():impl_(std::make_unique<Impl>()){}
View::~View(){close();}
Availability View::availability(const Options& options){return detail::platformAvailability(options);}
bool View::open(Options options){
    impl_->thread();close();detail::validateBounds(options.bounds);
    impl_->state=std::make_shared<detail::State>();impl_->state->options=std::move(options);
    if(!impl_->state->options.parent){impl_->state->push(Event::Kind::error,"Supply a native parent window");return false;}
    try{impl_->native=detail::createNative(impl_->state);return bool(impl_->native);}catch(const std::exception& e){impl_->state->push(Event::Kind::error,e.what());return false;}
}
bool View::ready()const{impl_->thread();return impl_->native&&impl_->state->ready&&!impl_->state->closed;}
bool View::loadHtml(std::string_view html){
    impl_->thread();if(!impl_->native||html.size()>detail::maxHtmlBytes||html.find('\0')!=html.npos)return false;
    std::string content;
    if(!impl_->state->options.allowRemoteContent)content=detail::offlineContentPolicy;
    content+=html;
    if(!ready()){impl_->state->pendingHtml=std::move(content);return true;}
    return impl_->native->loadHtml(content);
}
bool View::navigate(std::string_view uri){impl_->thread();if(!ready()||uri.size()>8192||uri.find('\0')!=uri.npos)return false;if(!detail::navigationAllowed(uri,impl_->state->options)){impl_->state->push(Event::Kind::navigationBlocked,std::string(uri));return false;}return impl_->native->navigate(uri);}
bool View::evaluate(std::string_view script){impl_->thread();return ready()&&script.size()<=1024u*1024&&script.find('\0')==script.npos&&impl_->native->evaluate(script);}
bool View::send(std::string_view json){impl_->thread();if(!detail::validJson(json))return false;return evaluate("window.__cellReceive("+std::string(json)+");");}
double View::parentPixelScale()const{impl_->thread();return impl_->native?impl_->native->parentPixelScale():1.;}
std::uintptr_t View::x11Window()const{impl_->thread();return impl_->native?impl_->native->x11Window():0;}
void View::setBounds(Bounds b){impl_->thread();detail::validateBounds(b);impl_->state->options.bounds=b;if(impl_->native)impl_->native->bounds(b);}
void View::setVisible(bool visible){impl_->thread();if(impl_->native)impl_->native->visible(visible);}
void View::focus(){impl_->thread();if(impl_->native)impl_->native->focus();}
std::vector<Event> View::poll(){
    impl_->thread();if(impl_->native)impl_->native->poll();
    if(ready()&&!impl_->state->pendingHtml.empty()){
        auto html=std::move(impl_->state->pendingHtml);impl_->state->pendingHtml.clear();
        if(!impl_->native->loadHtml(html)&&impl_->state->error.empty())
            impl_->state->push(Event::Kind::error,"Cannot load queued WebView HTML");
    }
    std::vector<Event> result;result.reserve(impl_->state->events.size());while(!impl_->state->events.empty()){result.push_back(std::move(impl_->state->events.front()));impl_->state->events.pop_front();}return result;
}
void View::close(){if(!impl_)return;impl_->thread();impl_->state->closed=true;impl_->state->ready=false;impl_->state->events.clear();impl_->state->pendingHtml.clear();if(impl_->native){impl_->native->close();impl_->native.reset();}}
std::string View::error()const{impl_->thread();return impl_->state->error;}
bool View::readyToUnload()noexcept{return detail::platformReadyToUnload();}
}
