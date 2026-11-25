#pragma once
#include <cell/webview.hpp>
#include <cell/detail/web_document.hpp>
#include <deque>
#include <functional>
#include <thread>
namespace cell::web::detail {
struct State {
    Options options;
    bool ready{},closed{};
    std::string error,pendingHtml;
    std::deque<Event> events;
    void push(Event::Kind,std::string);
};
class NativeView {
public:
    virtual ~NativeView()=default;
    virtual bool loadHtml(std::string_view)=0;
    virtual bool navigate(std::string_view)=0;
    virtual bool evaluate(std::string_view)=0;
    virtual double parentPixelScale()const{return 1.;}
    virtual std::uintptr_t x11Window()const{return 0;}
    virtual void bounds(Bounds)=0;
    virtual void visible(bool)=0;
    virtual void focus()=0;
    virtual void poll()=0;
    virtual void close()=0;
};
Availability platformAvailability(const Options&);
std::unique_ptr<NativeView> createNative(const std::shared_ptr<State>&);
bool platformReadyToUnload() noexcept;
bool navigationAllowed(std::string_view,const Options&) noexcept;
void validateBounds(Bounds);
bool validJson(std::string_view) noexcept;
const char* assetMime(std::string_view name) noexcept;
const Asset* findAsset(const Options&, std::string_view name) noexcept;
} // namespace cell::web::detail
