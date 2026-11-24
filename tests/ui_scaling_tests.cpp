#include <cell/ui_scaling.hpp>
#include <cell/hover_help.hpp>
#include <iostream>
#include <limits>
#include <chrono>
#include <thread>
#include <memory>
#if CELL_SCALING_X11
#include <X11/Xlib.h>
#include <X11/keysym.h>
#include <dlfcn.h>
#endif

using namespace cell::ui;
namespace {
#if CELL_SCALING_X11
struct DisplayCloser {void operator()(Display* display)const noexcept{if(display)XCloseDisplay(display);}};
using DisplayOwner=std::unique_ptr<Display,DisplayCloser>;
#endif
void check(bool value,const char* why){if(!value)throw std::runtime_error(why);}
bool same(Point a,Point b){return std::abs(a.x-b.x)<1e-9&&std::abs(a.y-b.y)<1e-9;}
void hoverHelp(){
    HoverHelp help;const auto start=HoverHelp::Clock::time_point{};
    help.observe({615,970},17,"Shift-drag to change the centre frequency. Regular dragging adjusts the tilt.",start);
    check(!help.tick(start+std::chrono::milliseconds(599))&&!help.visible(),"Hover help appeared before its delay");
    check(help.tick(start+std::chrono::milliseconds(600))&&help.visible(),"Hover delay did not request a repaint");
    help.observe({615,970},17,std::string(help.text()),start+std::chrono::milliseconds(700));
    check(!help.tick(start+std::chrono::milliseconds(800))&&help.visible(),"An unchanged repaint restarted hover help");
    Surface surface;surface.resize(620,980);Rect box;surface.render([&](Canvas& canvas){box=help.paint(canvas,{0,0,620,980});});
    check(box.x>=0&&box.y>=0&&box.x+box.width<=620&&box.y+box.height<=980,"Tooltip escaped the editor at the bottom-right edge");
    help.observe({10,10},18,"Another control",start+std::chrono::milliseconds(900));check(!help.visible(),"Moving to another control retained its tooltip");
    help.hide();help.tick(start+std::chrono::seconds(5));check(!help.visible()&&help.text().empty(),"Cancelled input reopened stale hover help");
}
void geometry(){
    for(const Point initial:{Point{620,980},Point{1001,761},Point{1320,760},Point{1280,920}}){
        ViewGeometry view(initial);
        for(unsigned cycle=0;cycle<100;++cycle)for(double scale:{1.,1.25,1.5,1.75,2.,2.5,3.,4.}){
            check(view.setScale(scale)&&view.resizeNative(view.nativeSize()),"Valid fractional content scale rejected");
            check(same(view.logicalSize(),initial),"Rounded scale acknowledgements accumulated logical-size drift");
            const Point logical{80,40};check(same(logicalPoint(nativePoint(logical,scale),scale),logical),"Content scale changed pointer identity");
        }
        const auto previous=view.nativeSize();
        for(double bad:{0.,-.5,9.,std::numeric_limits<double>::infinity(),std::numeric_limits<double>::quiet_NaN()})
            check(!view.setScale(bad)&&same(view.nativeSize(),previous),"Invalid scale changed the view");
        check(!view.resizeNative({8193,100})&&!view.resizeNative({0,10})&&same(view.nativeSize(),previous),"Invalid native dimensions changed the view");
    }
    ViewGeometry manual({1000,620});check(manual.setScale(1.25)&&manual.resizeNative({1499,1001}),"Manual fractional-scale resize rejected");const auto logical=manual.logicalSize();
    for(unsigned n=0;n<100;++n)for(double scale:{1.75,1.,2.,1.25})check(manual.setScale(scale)&&manual.resizeNative(manual.nativeSize())&&same(manual.logicalSize(),logical),"User size drifted while moving between display scales");
}
void backingPixels(){
    for(double content:{1.,1.25,1.5,1.75,2.})for(double backing:{1.,2.}){
        const double scale=content*backing;Surface surface;surface.resize(int(200*scale),int(120*scale));
        surface.render([&](Canvas& canvas){canvas.scale(scale,scale);canvas.fill({0,0,200,120},Color(0xff112233));canvas.fill({20,20,100,40},Color(0xffaabbcc));});
        const auto pixel=[&](unsigned x,unsigned y){return *reinterpret_cast<const std::uint32_t*>(static_cast<const std::byte*>(surface.pixels())+y*surface.stride()+x*4);};
        check(pixel(unsigned(80*scale),unsigned(40*scale))==0xffaabbcc&&pixel(unsigned(10*scale),unsigned(10*scale))==0xff112233,"Backing density changed logical control geometry");
    }
}
class TrackingEditor final:public Editor {
public:
    Rect last{};Point pointerAt{};unsigned cancellations{},downs{},ups{},paints{};
    void paint(Canvas& c,Rect bounds)override{++paints;last=bounds;c.fill(bounds,Color(0xff18232d));c.fill({20,20,100,40},Color(0xff75d3cb));}
    bool pointer(const PointerEvent& event)override{pointerAt=event.position;return true;}
    void cancelInput()override{++cancellations;}
    bool keyboard(const KeyEvent& event)override{if(event.key=='a')++downs;return event.key=='a';}
    bool keyReleased(const KeyEvent& event)override{if(event.key=='a')++ups;return event.key=='a';}
};
void embeddedFocusTransfer(bool nativeButtons=false){
#if CELL_SCALING_X11
    struct NativeInput {
        void* library{};
        using Button=int(*)(Display*,unsigned int,int,unsigned long);
        Button button{};
        ~NativeInput(){if(library)dlclose(library);}
    } input;
    if(nativeButtons){
        input.library=dlopen("libXtst.so.6",RTLD_NOW|RTLD_LOCAL);
        check(input.library,"Native input fixture requires the XTest runtime");
        input.button=reinterpret_cast<NativeInput::Button>(dlsym(input.library,"XTestFakeButtonEvent"));
        check(input.button,"Native input fixture requires XTest button injection");
    }
    struct FocusEditor final:Editor {
        bool editing{};unsigned presses{},cancellations{};
        void paint(Canvas& canvas,Rect bounds)override{canvas.fill(bounds,Color(0xff18232d));}
        bool pointer(const PointerEvent& event)override{if(event.action==PointerAction::down||event.action==PointerAction::context)editing=true;return true;}
        void cancelInput()override{editing=false;++cancellations;}
        bool keyboard(const KeyEvent& event)override{if(editing&&event.key=='a'){++presses;return true;}return false;}
    } editor;
    DisplayOwner hostOwner(XOpenDisplay(nullptr));auto* host=hostOwner.get();check(host,"Focus-transfer connection failed");
    const auto parent=XCreateSimpleWindow(host,DefaultRootWindow(host),0,0,400,300,0,0,0);
    const auto other=XCreateSimpleWindow(host,DefaultRootWindow(host),500,0,200,200,0,0,0);
    XMapWindow(host,parent);XMapWindow(host,other);XSync(host,False);
    {
        cell::ui::Window child(editor);check(child.open(parent,400,300),"Focus-transfer child failed");child.show(true);
        const auto pump=[&]{const auto until=std::chrono::steady_clock::now()+std::chrono::milliseconds(60);do{child.poll();XSync(host,False);std::this_thread::sleep_for(std::chrono::milliseconds(1));}while(std::chrono::steady_clock::now()<until);};
        pump();
        // With pointer focus in the child and explicit focus on its host, a
        // click that explicitly focuses the child produces NotifyPointer
        // FocusOut followed by FocusIn. This is entry, not a lost text edit.
        XWarpPointer(host,None,::Window(child.nativeHandle()),0,0,0,0,40,40);
        XSetInputFocus(host,parent,RevertToParent,CurrentTime);XSync(host,False);pump();
        XEvent click{};click.xbutton.type=ButtonPress;click.xbutton.display=host;click.xbutton.window=::Window(child.nativeHandle());click.xbutton.button=Button1;click.xbutton.x=40;click.xbutton.y=40;click.xbutton.same_screen=True;
        const auto press=[&]{
            if(nativeButtons){
                check(input.button(host,click.xbutton.button,True,CurrentTime),"Native button press failed");
                check(input.button(host,click.xbutton.button,False,CurrentTime),"Native button release failed");
            }else check(XSendEvent(host,click.xbutton.window,False,NoEventMask,&click),"Focus-transfer click failed");
            XSync(host,False);pump();
        };
        press();
        check(editor.editing,"Host-to-child focus transfer discarded the active text edit");
        XEvent key{};key.xkey.type=KeyPress;key.xkey.display=host;key.xkey.window=::Window(child.nativeHandle());key.xkey.keycode=XKeysymToKeycode(host,XK_a);key.xkey.same_screen=True;
        check(XSendEvent(host,key.xkey.window,False,NoEventMask,&key),"Focus-transfer key failed");XSync(host,False);pump();
        check(editor.presses==1,"Host-to-child focus transfer lost the subsequent key");
        const auto previous=editor.cancellations;
        XSetInputFocus(host,other,RevertToParent,CurrentTime);XSync(host,False);pump();
        check(!editor.editing&&editor.cancellations>previous,"Real external focus loss retained active input");
        for(const auto previousFocus:{other,parent}){
            XSetInputFocus(host,previousFocus,RevertToParent,CurrentTime);XSync(host,False);pump();
            ::Window beforeContext{};int beforeRevert{};XGetInputFocus(host,&beforeContext,&beforeRevert);
            check(beforeContext==previousFocus&&!editor.editing,"Native focus reclamation intercepted genuine focus loss");
            click.xbutton.button=Button3;
            press();
            ::Window focused{};int revert{};XGetInputFocus(host,&focused,&revert);
            check(focused==::Window(child.nativeHandle())&&editor.editing,"Context entry did not focus the embedded editor");
            // A host can activate its frame after the child requested focus.
            // Reproduce the real delayed activation seen in REAPER.
            XSetInputFocus(host,parent,RevertToParent,CurrentTime);XSync(host,False);pump();
            XGetInputFocus(host,&focused,&revert);
            check(focused==::Window(child.nativeHandle())&&editor.editing,"Host activation discarded context input");
            check(XSendEvent(host,key.xkey.window,False,NoEventMask,&key),"Context-entry key failed");XSync(host,False);pump();
        }
        check(editor.presses==3,"Context entry lost the subsequent key");
        // A recent click is not permission to pull focus back from a host
        // control after the pointer has left our embedded subtree.
        press();
        XWarpPointer(host,None,other,0,0,0,0,40,40);
        XSetInputFocus(host,parent,RevertToParent,CurrentTime);XSync(host,False);pump();
        ::Window afterExit{};int afterRevert{};XGetInputFocus(host,&afterExit,&afterRevert);
        check(afterExit==parent&&!editor.editing,"Native focus reclamation ignored the pointer leaving the editor");
        XSetInputFocus(host,::Window(child.nativeHandle()),RevertToParent,CurrentTime);XSync(host,False);pump();editor.editing=true;
        XSetInputFocus(host,other,RevertToParent,CurrentTime);
        XSetInputFocus(host,::Window(child.nativeHandle()),RevertToParent,CurrentTime);XSync(host,False);pump();
        check(!editor.editing,"A queued real focus loss was mistaken for pointer entry after refocusing");
        // A keyboard grab must still release notes/edits, even though the
        // server's explicit focus remains on our child during the grab.
        XSetInputFocus(host,::Window(child.nativeHandle()),RevertToParent,CurrentTime);XSync(host,False);pump();editor.editing=true;
        check(XGrabKeyboard(host,other,False,GrabModeAsync,GrabModeAsync,CurrentTime)==GrabSuccess,"Focus-transfer keyboard grab failed");XSync(host,False);pump();
        check(!editor.editing,"External keyboard grab retained active input");
        XUngrabKeyboard(host,CurrentTime);XSync(host,False);child.close();
    }
    XDestroyWindow(host,parent);XDestroyWindow(host,other);hostOwner.reset();
#else
    (void)nativeButtons;
#endif
}
void nativeWindow(bool nativeButtons=false){
#if CELL_SCALING_X11
    TrackingEditor editor;cell::ui::Window window(editor);check(window.open(0,1000,620),"Scaling native window could not open");window.show(true);window.poll();
    DisplayOwner displayOwner(XOpenDisplay(nullptr));auto* display=displayOwner.get();check(display,"Scaling input connection failed");
    for(double scale:{1.,1.25,1.5,1.75,2.}){
        const auto paints=editor.paints;const bool changed=window.contentScaleFactor()!=scale;
        check(window.setContentScaleFactor(scale)&&window.resize(int(1000*scale),int(620*scale)),"Native scale/resize failed");
        const auto deadline=std::chrono::steady_clock::now()+std::chrono::seconds(2);
        do{window.poll();if(editor.paints&&(!changed||editor.paints>paints)&&same({editor.last.width,editor.last.height},{1000,620}))break;std::this_thread::sleep_for(std::chrono::milliseconds(1));}while(std::chrono::steady_clock::now()<deadline);
        if(!editor.paints||(changed&&editor.paints==paints)||!same({editor.last.width,editor.last.height},{1000,620})) {
            XWindowAttributes a{};const bool known=XGetWindowAttributes(display,::Window(window.nativeHandle()),&a);
            throw std::runtime_error("Native logical bounds scale="+std::to_string(scale)+" painted="+std::to_string(editor.last.width)+"x"+std::to_string(editor.last.height)+" mapped="+std::to_string(known?a.map_state:-1)+" native="+std::to_string(known?a.width:0)+"x"+std::to_string(known?a.height:0));
        }
        const auto at=window.toNativePoint({80,40});check(same(at,{80*scale,40*scale}),"Host context-menu position used the wrong coordinate system");
        XEvent event{};event.xbutton.type=ButtonPress;event.xbutton.display=display;event.xbutton.window=::Window(window.nativeHandle());event.xbutton.button=Button1;event.xbutton.x=int(at.x);event.xbutton.y=int(at.y);event.xbutton.same_screen=True;
        check(XSendEvent(display,event.xbutton.window,False,NoEventMask,&event),"Scaled native event injection failed");XSync(display,False);window.poll();
        check(same(editor.pointerAt,{80,40}),"Native pointer missed its logical control at fractional scale");
    }
    check(!window.setContentScaleFactor(0)&&window.contentScaleFactor()==2,"Invalid native scale changed the renderer");
    check(editor.cancellations>=4,"Scale changes retained an active pointer coordinate system");
    XEvent key{};key.xkey.display=display;key.xkey.window=::Window(window.nativeHandle());key.xkey.keycode=XKeysymToKeycode(display,XK_a);key.xkey.same_screen=True;
    for(int type:{KeyPress,KeyRelease}){key.xkey.type=type;check(XSendEvent(display,key.xkey.window,False,NoEventMask,&key),"Native key event injection failed");}XSync(display,False);window.poll();check(editor.downs==1&&editor.ups==1,"Native window did not deliver a distinct key press and release");
    displayOwner.reset();window.show(false);window.close();
    DisplayOwner hostOwner(XOpenDisplay(nullptr));auto* host=hostOwner.get();check(host,"Host key forwarding connection failed");const auto parent=XCreateSimpleWindow(host,DefaultRootWindow(host),0,0,1000,620,0,0,0);XSelectInput(host,parent,KeyPressMask|KeyReleaseMask);XMapWindow(host,parent);XSync(host,False);
    TrackingEditor child;cell::ui::Window embedded(child);check(embedded.open(std::uintptr_t(parent),1000,620),"Embedded forwarding window failed");embedded.show(true);embedded.poll();
    XEvent space{};space.xkey.display=host;space.xkey.window=::Window(embedded.nativeHandle());space.xkey.keycode=XKeysymToKeycode(host,XK_space);space.xkey.same_screen=True;
    for(int type:{KeyPress,KeyRelease}){space.xkey.type=type;check(XSendEvent(host,space.xkey.window,False,NoEventMask,&space),"Host key injection failed");}XSync(host,False);embedded.poll();
    unsigned presses=0,releases=0;const auto deadline=std::chrono::steady_clock::now()+std::chrono::seconds(1);
    while((!presses||!releases)&&std::chrono::steady_clock::now()<deadline){while(XPending(host)){XEvent event{};XNextEvent(host,&event);if(event.type==KeyPress)++presses;if(event.type==KeyRelease)++releases;}if(!presses||!releases)std::this_thread::sleep_for(std::chrono::milliseconds(1));}
    check(presses==1&&releases==1,"Unconsumed Space did not return to the plugin host");embedded.close();XDestroyWindow(host,parent);hostOwner.reset();
    embeddedFocusTransfer(nativeButtons);
#else
    (void)nativeButtons;
    throw std::runtime_error("Native scaling fixture requires X11");
#endif
}
}
int main(int argc,char** argv){try{if(argc>1&&std::string_view(argv[1])=="--native")nativeWindow();else if(argc>1&&std::string_view(argv[1])=="--native-input")nativeWindow(true);else{geometry();backingPixels();hoverHelp();}std::cout<<"PASS logical/native/backing coordinate separation, fractional zoom, drift-free resize and scale validation\n";}catch(const std::exception& e){std::cerr<<e.what()<<'\n';return 1;}}
