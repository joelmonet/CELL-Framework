#include "internal.hpp"
#include <dlfcn.h>
#include <algorithm>
#include <array>
#include <atomic>
#include <cstring>
#include <stdexcept>
#include <type_traits>
namespace cell::web::detail {
namespace {
struct LibraryClose {void operator()(void* p) const noexcept{if(p)dlclose(p);}};
using Library=std::unique_ptr<void,LibraryClose>;
struct Runtime {
    Library gtk,webkit;
    std::string error;
    int (*init)(int*,char***){};
    void* (*plugNew)(void*,unsigned long){};
    void (*add)(void*,void*){};
    void (*show)(void*){};void (*hide)(void*){};void (*destroy)(void*){};
    void (*realize)(void*){};void* (*window)(void*){};
    void (*moveResize)(void*,int,int,int,int){};
    struct Allocation {int x,y,width,height;};
    void (*allocate)(void*,const Allocation*){};
    void (*windowFocus)(void*,unsigned){};
    int (*scaleFactor)(void*){};
    void* (*defaultDisplay)(){};
    const char* (*displayName)(void*){};
    void* (*displayOpen)(const char*){};
    void (*displayClose)(void*){};
    void* (*defaultScreen)(void*){};
    void (*setScreen)(void*,void*){};
    void* (*lookupWindow)(void*,unsigned long){};
    void (*windowUserData)(void*,void**){};
    std::size_t (*socketType)(){};
    std::size_t (*x11DisplayType)(){};
    int (*isInstanceOf)(void*,std::size_t){};
    void* (*display)(void*){};void* (*xdisplay)(void*){};unsigned long (*xid)(void*){};
    int (*xmove)(void*,unsigned long,int,int,unsigned,unsigned){};int (*xflush)(void*){};
    void (*sizeRequest)(void*,int,int){};
    void (*windowResize)(void*,int,int){};
    void (*grabFocus)(void*){};
    void* (*refSink)(void*){};void (*unref)(void*){};void (*free)(void*){};
    unsigned long (*connect)(void*,const char*,void(*)(),void*,void(*)(void*,void*),unsigned){};
    void (*disconnect)(void*,unsigned long){};
    int (*iterate)(void*,int){};
    void* (*dataEphemeral)(){};void* (*contextNew)(void*){};
    void* (*viewNew)(void*){};void* (*manager)(void*){};
    void* (*settings)(void*){};void (*developerTools)(void*,int){};
    int (*registerMessage)(void*,const char*){};void (*unregisterMessage)(void*,const char*){};
    void* (*scriptNew)(const char*,int,int,const char* const*,const char* const*){};
    void (*scriptAdd)(void*,void*){};void (*scriptUnref)(void*){};
    void (*loadHtml)(void*,const char*,const char*){};void (*loadUri)(void*,const char*){};
    void (*evaluate)(void*,const char*,std::intptr_t,const char*,const char*,void*,void(*)(void*,void*,void*),void*){};
    void* (*jsValue)(void*){};char* (*jsString)(void*){};
    void* (*request)(void*){};const char* (*uri)(void*){};void (*ignore)(void*){};
    bool loaded{};
    Runtime(){
        // GTK 2 and GTK 3 register conflicting types in one process. The
        // exported version variable also identifies hosts with a GTK 2 fork.
        if (const auto* major = static_cast<const unsigned*>(dlsym(RTLD_DEFAULT, "gtk_major_version"));
            major && *major == 2) {
            error="This host uses GTK 2. The editor requires a GTK 3 compatible host";
            return;
        }
        // GTK/WebKit own process-wide threads and type registrations. NODELETE
        // retains their code mappings after the final widget, as required by
        // those runtimes; Cell signal handlers are still disconnected on close.
        gtk.reset(dlopen("libgtk-3.so.0",RTLD_NOW|RTLD_LOCAL|RTLD_NODELETE));
        webkit.reset(dlopen("libwebkit2gtk-4.1.so.0",RTLD_NOW|RTLD_LOCAL|RTLD_NODELETE));
        if(!gtk||!webkit){error="Install GTK 3 and WebKitGTK 4.1 (libwebkit2gtk-4.1-0)";return;}
        auto bind=[&](auto& fn,const char* name){void* p=dlsym(webkit.get(),name);if(!p)p=dlsym(gtk.get(),name);fn=reinterpret_cast<std::remove_reference_t<decltype(fn)>>(p);if(!p)error=std::string("Missing system WebView symbol: ")+name;return p!=nullptr;};
        loaded=bind(init,"gtk_init_check")&&bind(plugNew,"gtk_plug_new_for_display")&&bind(add,"gtk_container_add")&&
          bind(show,"gtk_widget_show_all")&&bind(hide,"gtk_widget_hide")&&bind(destroy,"gtk_widget_destroy")&&
          bind(realize,"gtk_widget_realize")&&bind(window,"gtk_widget_get_window")&&bind(moveResize,"gdk_window_move_resize")&&
          bind(allocate,"gtk_widget_size_allocate")&&bind(windowFocus,"gdk_window_focus")&&bind(scaleFactor,"gtk_widget_get_scale_factor")&&
          bind(defaultDisplay,"gdk_display_get_default")&&bind(x11DisplayType,"gdk_x11_display_get_type")&&bind(isInstanceOf,"g_type_check_instance_is_a")&&
          bind(displayName,"gdk_display_get_name")&&bind(displayOpen,"gdk_display_open")&&bind(displayClose,"gdk_display_close")&&
          bind(defaultScreen,"gdk_display_get_default_screen")&&bind(setScreen,"gtk_window_set_screen")&&
          bind(lookupWindow,"gdk_x11_window_lookup_for_display")&&bind(windowUserData,"gdk_window_get_user_data")&&bind(socketType,"gtk_socket_get_type")&&
          bind(display,"gdk_window_get_display")&&bind(xdisplay,"gdk_x11_display_get_xdisplay")&&bind(xid,"gdk_x11_window_get_xid")&&bind(xmove,"XMoveResizeWindow")&&bind(xflush,"XFlush")&&bind(sizeRequest,"gtk_widget_set_size_request")&&bind(windowResize,"gtk_window_resize")&&bind(grabFocus,"gtk_widget_grab_focus")&&
          bind(refSink,"g_object_ref_sink")&&bind(unref,"g_object_unref")&&bind(free,"g_free")&&bind(connect,"g_signal_connect_data")&&bind(disconnect,"g_signal_handler_disconnect")&&bind(iterate,"g_main_context_iteration")&&
          bind(dataEphemeral,"webkit_website_data_manager_new_ephemeral")&&bind(contextNew,"webkit_web_context_new_with_website_data_manager")&&bind(viewNew,"webkit_web_view_new_with_context")&&
          bind(manager,"webkit_web_view_get_user_content_manager")&&bind(settings,"webkit_web_view_get_settings")&&bind(developerTools,"webkit_settings_set_enable_developer_extras")&&
          bind(registerMessage,"webkit_user_content_manager_register_script_message_handler")&&bind(unregisterMessage,"webkit_user_content_manager_unregister_script_message_handler")&&
          bind(scriptNew,"webkit_user_script_new")&&bind(scriptAdd,"webkit_user_content_manager_add_script")&&bind(scriptUnref,"webkit_user_script_unref")&&
          bind(loadHtml,"webkit_web_view_load_html")&&bind(loadUri,"webkit_web_view_load_uri")&&bind(evaluate,"webkit_web_view_evaluate_javascript")&&
          bind(jsValue,"webkit_javascript_result_get_js_value")&&bind(jsString,"jsc_value_to_string")&&bind(request,"webkit_navigation_policy_decision_get_request")&&bind(uri,"webkit_uri_request_get_uri")&&bind(ignore,"webkit_policy_decision_ignore");
    }
};
Runtime& runtime(){static Runtime value;return value;}
std::atomic<unsigned> views{};
std::thread::id guiThread;
class LinuxView final:public NativeView {
    Runtime& api_=runtime();std::shared_ptr<State> state_;
    void* privateDisplay_{};
    void* plug_{};void* web_{};void* manager_{};
    unsigned long messageSignal_{},loadSignal_{},policySignal_{};
    bool counted_{};
    static void message(void*,void* result,void* context) noexcept {
        auto& self=*static_cast<LinuxView*>(context);
        try{char* value=self.api_.jsString(self.api_.jsValue(result));if(value){const auto length=strnlen(value,maxMessageBytes+1);std::string text=length>maxMessageBytes?std::string{}:std::string(value,length);self.api_.free(value);if(length>maxMessageBytes){self.state_->push(Event::Kind::error,"WebView bridge message bound");return;}self.state_->push(Event::Kind::message,std::move(text));}}catch(...){}
    }
    static void loaded(void*,int event,void* context) noexcept {
        if(event==3)try{static_cast<LinuxView*>(context)->state_->push(Event::Kind::loaded,{});}catch(...){}
    }
    static int policy(void*,void* decision,int kind,void* context) noexcept {
        auto& self=*static_cast<LinuxView*>(context);
        if(kind==1){self.api_.ignore(decision);return 1;} // new native windows belong to the caller
        if(kind!=0)return 0;
        const char* uri=self.api_.uri(self.api_.request(decision));
        if(uri&&!navigationAllowed(uri,self.state_->options)){
            self.api_.ignore(decision);try{self.state_->push(Event::Kind::navigationBlocked,uri);}catch(...){}return 1;
        }
        return 0;
    }
public:
    explicit LinuxView(std::shared_ptr<State> state):state_(std::move(state)){
        if(!api_.loaded)throw std::runtime_error(api_.error);
        if(guiThread!=std::thread::id{}&&guiThread!=std::this_thread::get_id())throw std::runtime_error("GTK WebViews require the same GUI thread");
        if(!api_.init(nullptr,nullptr))throw std::runtime_error("Cannot initialize GTK on the GUI display");
        auto* display = api_.defaultDisplay();
        if (!display || !api_.isInstanceOf(display, api_.x11DisplayType()))
            throw std::runtime_error("The GTK display must use X11 to embed a Cell WebView");
        guiThread=std::this_thread::get_id();
        try{
            const auto parent = static_cast<unsigned long>(state_->options.parent);
            if (auto* localWindow = api_.lookupWindow(display, parent)) {
                void* widget{};
                api_.windowUserData(localWindow, &widget);
                if (widget && !api_.isInstanceOf(widget, api_.socketType())) {
                    // Suil supplies a GtkPlug window. GtkPlug accepts this as
                    // an X11 parent when it belongs to another connection.
                    privateDisplay_ = api_.displayOpen(api_.displayName(display));
                    if (!privateDisplay_) throw std::runtime_error("Cannot connect to the X11 parent display");
                    display = privateDisplay_;
                }
            }
            plug_=api_.plugNew(display,parent);if(!plug_)throw std::runtime_error("Cannot create GTK WebView child");api_.refSink(plug_);
            if (privateDisplay_) api_.setScreen(plug_, api_.defaultScreen(privateDisplay_));
            auto* data=api_.dataEphemeral();if(!data)throw std::runtime_error("Cannot create private WebKit storage");
            auto* context=api_.contextNew(data);api_.unref(data);if(!context)throw std::runtime_error("Cannot create WebKit context");
            web_=api_.viewNew(context);api_.unref(context);if(!web_)throw std::runtime_error("Cannot create WebKit view");api_.refSink(web_);
            api_.add(plug_,web_);manager_=api_.manager(web_);
            messageSignal_=api_.connect(manager_,"script-message-received::cell",reinterpret_cast<void(*)()>(&message),this,nullptr,0);
            if(!api_.registerMessage(manager_,"cell"))throw std::runtime_error("Cannot register WebKit bridge");
            const std::string script(bridgeScript());auto* bootstrap=api_.scriptNew(script.c_str(),0,0,nullptr,nullptr);
            if(!bootstrap)throw std::runtime_error("Cannot create WebKit bootstrap");api_.scriptAdd(manager_,bootstrap);api_.scriptUnref(bootstrap);
            api_.developerTools(api_.settings(web_),state_->options.developerTools?1:0);
            loadSignal_=api_.connect(web_,"load-changed",reinterpret_cast<void(*)()>(&loaded),this,nullptr,0);
            policySignal_=api_.connect(web_,"decide-policy",reinterpret_cast<void(*)()>(&policy),this,nullptr,0);
            api_.realize(plug_);bounds(state_->options.bounds);api_.show(plug_);
            state_->ready=true;state_->push(Event::Kind::ready,{});++views;counted_=true;
        }catch(...){close();throw;}
    }
    ~LinuxView()override{close();}
    bool loadHtml(std::string_view html)override{if(!web_)return false;std::string text(html);api_.loadHtml(web_,text.c_str(),"about:blank");return true;}
    bool navigate(std::string_view uri)override{if(!web_)return false;std::string text(uri);api_.loadUri(web_,text.c_str());return true;}
    bool evaluate(std::string_view script)override{if(!web_)return false;std::string text(script);api_.evaluate(web_,text.c_str(),std::intptr_t(text.size()),nullptr,nullptr,nullptr,nullptr,nullptr);return true;}
    void bounds(Bounds b)override{
        if(!plug_)return;const int scale=std::max(1,api_.scaleFactor(plug_));
        const int w=std::max(1,(b.width+scale-1)/scale),h=std::max(1,(b.height+scale-1)/scale);
        if(!privateDisplay_)api_.sizeRequest(web_,w,h);
        api_.windowResize(plug_,w,h);
        const Runtime::Allocation allocation{0,0,w,h};api_.allocate(plug_,&allocation);api_.allocate(web_,&allocation);
        if(auto* window=api_.window(plug_)){api_.moveResize(window,b.x/scale,b.y/scale,w,h);auto* display=api_.xdisplay(api_.display(window));api_.xmove(display,api_.xid(window),b.x,b.y,unsigned(b.width),unsigned(b.height));api_.xflush(display);}
    }
    void visible(bool show)override{if(plug_){if(show)api_.show(plug_);else api_.hide(plug_);}}
    std::uintptr_t x11Window()const override{
        auto* window=plug_?api_.window(plug_):nullptr;
        return window?api_.xid(window):0;
    }
    void focus()override{if(web_){if(auto* w=api_.window(plug_))api_.windowFocus(w,0);api_.grabFocus(web_);}}
    void poll()override{
        // A GTK iteration can dispatch host work that closes this view.
        const auto state=state_;
        const auto iterate=api_.iterate;
        for(unsigned n=0;n<32&&!state->closed&&iterate(nullptr,0);++n){}
    }
    void close()override{
        if(manager_){if(messageSignal_)api_.disconnect(manager_,messageSignal_);api_.unregisterMessage(manager_,"cell");}
        if(web_){if(loadSignal_)api_.disconnect(web_,loadSignal_);if(policySignal_)api_.disconnect(web_,policySignal_);}
        messageSignal_=loadSignal_=policySignal_=0;manager_=nullptr;auto* heldWeb=web_;web_=nullptr;
        if(plug_){api_.destroy(plug_);api_.unref(plug_);plug_=nullptr;}
        if(heldWeb)api_.unref(heldWeb);
        if(privateDisplay_){api_.displayClose(privateDisplay_);privateDisplay_=nullptr;}
        if(counted_){--views;counted_=false;}
    }
};
}
Availability platformAvailability(const Options&){auto& api=runtime();return {Backend::webKitGtk,api.loaded,api.loaded?"WebKitGTK 4.1 / GTK 3 (X11 child)":api.error};}
std::unique_ptr<NativeView> createNative(const std::shared_ptr<State>& state){return std::make_unique<LinuxView>(state);}
bool platformReadyToUnload()noexcept{return views.load()==0;}
}
