#import <Cocoa/Cocoa.h>
#import <WebKit/WebKit.h>
#import <objc/runtime.h>
#import <objc/message.h>
#include "internal.hpp"
#include <atomic>
#include <cmath>
#include <cstring>
#include <stdexcept>
namespace cell::web::detail {
namespace {
std::atomic<unsigned> views{},delegates{};
Class delegateClass{};
struct DelegateData {std::weak_ptr<State> state;};
void setData(id object,DelegateData* data){const auto field=class_getInstanceVariable(object_getClass(object),"_cellState");std::memcpy(reinterpret_cast<char*>(object)+ivar_getOffset(field),&data,sizeof(data));}
DelegateData* getData(id object){DelegateData* data{};const auto field=class_getInstanceVariable(object_getClass(object),"_cellState");std::memcpy(&data,reinterpret_cast<char*>(object)+ivar_getOffset(field),sizeof(data));return data;}
void message(id self,SEL,id,WKScriptMessage* input){@autoreleasepool{
 try{if(auto* data=getData(self))if(auto state=data->state.lock();state&&!state->closed){
  if(![input.body isKindOfClass:[NSString class]]||[(NSString*)input.body length]>maxMessageBytes){state->push(Event::Kind::error,"WKWebView bridge message type/bounds");return;}
  const char* text=[(NSString*)input.body UTF8String];state->push(Event::Kind::message,text?text:"");
 }}catch(...){}
}}
void loaded(id self,SEL,WKWebView*,WKNavigation*){try{if(auto* data=getData(self))if(auto s=data->state.lock())s->push(Event::Kind::loaded,{});}catch(...){}}
void failed(id self,SEL,WKWebView*,WKNavigation*,NSError* error){try{if(auto* data=getData(self))if(auto s=data->state.lock())s->push(Event::Kind::error,[[error localizedDescription] UTF8String]);}catch(...){}}
void policy(id self,SEL,WKWebView*,WKNavigationAction* action,void(^decision)(WKNavigationActionPolicy)){
 bool allowed=false;
 try{if(auto* data=getData(self))if(auto s=data->state.lock();s&&!s->closed){const char* uri=[action.request.URL.absoluteString UTF8String];allowed=uri&&navigationAllowed(uri,s->options);if(!allowed)s->push(Event::Kind::navigationBlocked,uri?uri:"");}}catch(...){}
 decision(allowed?WKNavigationActionPolicyAllow:WKNavigationActionPolicyCancel);
}
void destroyDelegate(id self,SEL){delete getData(self);setData(self,nullptr);struct objc_super base{self,class_getSuperclass(object_getClass(self))};reinterpret_cast<void(*)(objc_super*,SEL)>(objc_msgSendSuper)(&base,sel_registerName("dealloc"));--delegates;}
Class makeDelegateClass(){
 if(delegateClass)return delegateClass;
 // A distinct runtime name per linked SDK image avoids Objective-C class
 // collisions when several independent plugin DLLs use this static library.
 const auto name=std::string("CellWebDelegate_")+std::to_string(reinterpret_cast<std::uintptr_t>(&makeDelegateClass));
 auto cls=objc_allocateClassPair([NSObject class],name.c_str(),0);if(!cls)throw std::runtime_error("Cannot allocate WebView delegate class");
 if(!class_addIvar(cls,"_cellState",sizeof(void*),std::uint8_t(std::log2(alignof(void*))),"^v")){objc_disposeClassPair(cls);throw std::runtime_error("Cannot allocate WebView delegate state");}
 class_addProtocol(cls,@protocol(WKScriptMessageHandler));class_addProtocol(cls,@protocol(WKNavigationDelegate));
 class_addMethod(cls,sel_registerName("userContentController:didReceiveScriptMessage:"),reinterpret_cast<IMP>(&message),"v@:@@");
 class_addMethod(cls,sel_registerName("webView:didFinishNavigation:"),reinterpret_cast<IMP>(&loaded),"v@:@@");
 class_addMethod(cls,sel_registerName("webView:didFailNavigation:withError:"),reinterpret_cast<IMP>(&failed),"v@:@@@");
 class_addMethod(cls,sel_registerName("webView:didFailProvisionalNavigation:withError:"),reinterpret_cast<IMP>(&failed),"v@:@@@");
 class_addMethod(cls,sel_registerName("webView:decidePolicyForNavigationAction:decisionHandler:"),reinterpret_cast<IMP>(&policy),"v@:@@@?");
 class_addMethod(cls,sel_registerName("dealloc"),reinterpret_cast<IMP>(&destroyDelegate),"v@:");objc_registerClassPair(cls);delegateClass=cls;return cls;
}
template<class T>struct OwnedObject {T value{};~OwnedObject(){[value release];}};
NSString* string(std::string_view text){return [[[NSString alloc] initWithBytes:text.data() length:text.size() encoding:NSUTF8StringEncoding] autorelease];}
class MacView final:public NativeView {
 std::shared_ptr<State> state_;NSView* parent_{};WKWebView* view_{};id delegate_{};bool counted_{};
public:
 explicit MacView(std::shared_ptr<State> state):state_(std::move(state)){
  if(![NSThread isMainThread])throw std::runtime_error("WKWebView requires the application GUI thread");
  parent_=reinterpret_cast<NSView*>(state_->options.parent);if(![parent_ isKindOfClass:[NSView class]]){parent_=nil;throw std::invalid_argument("WKWebView parent must be NSView*");}[parent_ retain];
  @autoreleasepool{
   try{
    delegate_=[[makeDelegateClass() alloc] init];if(!delegate_)throw std::runtime_error("Cannot create WKWebView delegate");++delegates;setData(delegate_,new DelegateData{state_});
    OwnedObject<WKWebViewConfiguration*> config{[[WKWebViewConfiguration alloc] init]};if(!config.value)throw std::runtime_error("Cannot create WKWebView configuration");config.value.websiteDataStore=[WKWebsiteDataStore nonPersistentDataStore];
    OwnedObject<WKUserContentController*> manager{[[WKUserContentController alloc] init]};if(!manager.value)throw std::runtime_error("Cannot create WKWebView content manager");[manager.value addScriptMessageHandler:delegate_ name:@"cell"];
    OwnedObject<WKUserScript*> script{[[WKUserScript alloc] initWithSource:string(bridgeScript()) injectionTime:WKUserScriptInjectionTimeAtDocumentStart forMainFrameOnly:YES]};if(!script.value)throw std::runtime_error("Cannot create WKWebView bridge");[manager.value addUserScript:script.value];
    config.value.userContentController=manager.value;
    view_=[[WKWebView alloc] initWithFrame:NSMakeRect(0,0,1,1) configuration:config.value];if(!view_)throw std::runtime_error("Cannot create WKWebView");
    view_.navigationDelegate=delegate_;if(@available(macOS 13.3,*))view_.inspectable=state_->options.developerTools;
    [parent_ addSubview:view_];bounds(state_->options.bounds);state_->ready=true;state_->push(Event::Kind::ready,{});++views;counted_=true;
   }catch(...){close();throw;}
  }
 }
 ~MacView()override{close();}
 bool loadHtml(std::string_view html)override{@autoreleasepool{auto* value=string(html);if(!view_||!value)return false;[view_ loadHTMLString:value baseURL:[NSURL URLWithString:@"about:blank"]];return true;}}
 bool navigate(std::string_view uri)override{@autoreleasepool{NSURL* url=[NSURL URLWithString:string(uri)];if(!view_||!url)return false;[view_ loadRequest:[NSURLRequest requestWithURL:url]];return true;}}
 bool evaluate(std::string_view code)override{@autoreleasepool{auto* value=string(code);if(!view_||!value)return false;[view_ evaluateJavaScript:value completionHandler:nil];return true;}}
 double parentPixelScale()const override{return parent_.window?parent_.window.backingScaleFactor:1.;}
 void bounds(Bounds b)override{
  if(!view_)return;
  const double scale=state_->options.boundsUnits==BoundsUnits::parentCoordinates?1.:parentPixelScale();
  const double x=b.x/scale,y=b.y/scale,w=b.width/scale,h=b.height/scale;
  const auto parentBounds=parent_.bounds;
  view_.frame=NSMakeRect(NSMinX(parentBounds)+x,parent_.isFlipped?NSMinY(parentBounds)+y:NSMaxY(parentBounds)-y-h,w,h);
 }
 void visible(bool show)override{if(view_)view_.hidden=!show;}
 void focus()override{if(view_)[view_.window makeFirstResponder:view_];}
 void poll()override{} // Cocoa's host event loop owns callbacks.
 void close()override{@autoreleasepool{
  if(delegate_){delete getData(delegate_);setData(delegate_,nullptr);}
  if(view_){view_.navigationDelegate=nil;[view_.configuration.userContentController removeScriptMessageHandlerForName:@"cell"];[view_ stopLoading];[view_ removeFromSuperview];[view_ release];view_=nil;}
  [delegate_ release];delegate_=nil;[parent_ release];parent_=nil;if(counted_){--views;counted_=false;}
 }}
};
}
Availability platformAvailability(const Options&){return {Backend::wkWebView,true,"System WKWebView (macOS); runtime qualification required"};}
std::unique_ptr<NativeView> createNative(const std::shared_ptr<State>& state){return std::make_unique<MacView>(state);}
bool platformReadyToUnload()noexcept{
 if(views.load()||delegates.load())return false;
 if(delegateClass){if(![NSThread isMainThread])return false;objc_disposeClassPair(delegateClass);delegateClass=nullptr;}return true;
}
}
