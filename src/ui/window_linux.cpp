#include <cell/ui.hpp>
#include <cell/ui_scaling.hpp>
#include "platform_transfer.hpp"
#include "x11_checked.hpp"
#include <X11/Xlib.h>
#include <X11/Xutil.h>
#include <X11/keysym.h>
#include <X11/XKBlib.h>
#include <X11/cursorfont.h>
#include <bit>
#include <chrono>
#ifndef CELL_HAS_XI2
#define CELL_HAS_XI2 0
#endif
#if CELL_HAS_XI2
#include <X11/extensions/XInput2.h>
#endif

namespace cell::ui {
struct Window::Impl {
    Editor& editor;Surface surface;
    Display* display{};::Window handle{};std::uint32_t gc{};Atom closeAtom{};bool xi2{};int xiOpcode{};
    Visual*visual{};int depth{};std::vector<unsigned char>convertedPixels;
    ::Window embedParent{};std::array<bool,256> forwardedKeys{};
    bool alive=true,dirty=true,visible{},requestedVisible{},closePending{};unsigned dispatchDepth{};unsigned long lastClick{};int lastClickX{},lastClickY{};
    explicit Impl(Editor& e):editor(e) { editor.invalidate=[this]{dirty=true;}; }
    double contentScale=1;
    unsigned long resizeRequestSerial{};
    bool resizePending{};
    std::chrono::steady_clock::time_point resizeDeadline{};
    bool pointerGrabbed{};int pointerDevice{};Cursor cursor{};int cursorType=-1;
    std::chrono::steady_clock::time_point focusRequestUntil{};
    void pointer(PointerEvent event,unsigned modifiers=0){event.position=logicalPoint(event.position,contentScale);event.control=bool(modifiers&ControlMask);event.alt=bool(modifiers&Mod1Mask);event.command=bool(modifiers&Mod4Mask);editor.pointer(event);}
    void releasePointer(){if(!display)return;
#if CELL_HAS_XI2
        if(pointerDevice){checked::xiUngrab(display,pointerDevice,CurrentTime);pointerDevice=0;}
#endif
        if(pointerGrabbed){::cell::ui::checked::ungrabPointer(display,CurrentTime);pointerGrabbed=false;}}
    void updateCursor(){const int type=editor.pointerCursor();if(type==cursorType||!display||!handle)return;const unsigned shape=type==2?XC_watch:type==3?XC_xterm:type==4?XC_crosshair:type==5?XC_plus:type==6?XC_hand2:type==7?XC_fleur:type==8?XC_sb_h_double_arrow:type==9?XC_sb_v_double_arrow:type>=10?XC_fleur:XC_left_ptr;auto next=::cell::ui::checked::createCursor(display,shape);::cell::ui::checked::defineCursor(display,handle,next);if(cursor)::cell::ui::checked::freeCursor(display,cursor);cursor=next;cursorType=type;}

    void focusIfViewable(){
        // Mapping a managed window is asynchronous. A queued pointer event can
        // also outlive a hide request, so our desired visibility is insufficient
        // to decide whether XSetInputFocus is legal on the server.
        if(!display||!handle||!visible)return;
        XWindowAttributes attributes{};
        if(::cell::ui::checked::attributes(display,handle,&attributes)&&attributes.map_state==IsViewable){
            focusRequestUntil=embedParent?std::chrono::steady_clock::now()+std::chrono::seconds(1):std::chrono::steady_clock::time_point{};
            ::cell::ui::checked::setInputFocus(display,handle,RevertToParent,CurrentTime);
        }
    }
    bool pointerFocusInHostAncestor(::Window focused){
        if(!embedParent||focused==None||focused==PointerRoot||focused==handle)return false;
        // Query our own ancestor chain before following the pointer. An
        // unrelated active application must never be treated as our host.
        ::Window at=handle;bool ancestor=false;
        for(unsigned n=0;n<64;++n){
            ::Window root{},parent{},*children{};unsigned count{};
            const bool okay=::cell::ui::checked::queryTree(display,at,&root,&parent,&children,&count);if(children)XFree(children);
            if(!okay||!parent||parent==at)break;
            if(parent==focused){ancestor=true;break;}
            if(parent==root)break;at=parent;
        }
        if(!ancestor)return false;
        at=focused;
        for(unsigned n=0;n<64;++n){
            ::Window root{},child{};int rootX{},rootY{},x{},y{};unsigned mask{};
            if(!::cell::ui::checked::queryPointer(display,at,&root,&child,&rootX,&rootY,&x,&y,&mask)||!child)return false;
            if(child==handle)return true;at=child;
        }
        return false;
    }

    void forwardKey(XEvent event){
        if(!embedParent)return;event.xkey.window=embedParent;event.xkey.subwindow=None;
        ::cell::ui::checked::sendEvent(display,embedParent,True,event.type==KeyPress?KeyPressMask:KeyReleaseMask,&event);XFlush(display);
    }
    ~Impl(){close();}
    void close() {
        editor.nativeWindowHandle=0;
        if(dispatchDepth){closePending=true;visible=false;return;}
        closePending=false;
        if(display) {
            releasePointer();if(cursor){::cell::ui::checked::freeCursor(display,cursor);cursor=0;cursorType=-1;}
            if(gc)checked::freeGC(display,gc);
            if(handle){native::unregisterDropTarget(handle);checked::destroyWindow(display,handle);}
            XCloseDisplay(display);
        }
        display=nullptr;handle=0;gc=0;visual=nullptr;depth=0;visible=false;requestedVisible=false;focusRequestUntil={};
    }
#if CELL_HAS_XI2
    bool selectTouch(){
        int event{},error{},major=2,minor=2;if(!XQueryExtension(display,"XInputExtension",&xiOpcode,&event,&error)||XIQueryVersion(display,&major,&minor)!=Success||major<2||(major==2&&minor<2))return false;
        unsigned char mask[XIMaskLen(XI_LASTEVENT)]{};for(int type:{XI_TouchBegin,XI_TouchUpdate,XI_TouchEnd,XI_ButtonPress,XI_ButtonRelease,XI_Motion})XISetMask(mask,type);
        XIEventMask selection{XIAllMasterDevices,int(sizeof(mask)),mask};return checked::xiSelect(display,handle,XIAllMasterDevices,{mask,sizeof(mask)});
    }
    void xiPointer(const XIDeviceEvent& event,int type){
        const Point at{event.event_x,event.event_y};const bool fine=bool(event.mods.effective&ShiftMask);
        if(type==XI_TouchBegin||type==XI_TouchUpdate||type==XI_TouchEnd){if(type==XI_TouchBegin)focusIfViewable();const PointerIdentity id{(std::uint64_t(std::uint32_t(event.sourceid))<<32)|std::uint32_t(event.detail),PointerKind::touch};pointer({type==XI_TouchBegin?PointerAction::down:type==XI_TouchEnd?PointerAction::up:PointerAction::move,at,0,fine,false,id},event.mods.effective);return;}
        if(event.flags&XIPointerEmulated)return;
        if(type==XI_Motion){pointer({PointerAction::move,at,0,fine},event.mods.effective);return;}
        if(event.detail==Button4||event.detail==Button5){if(type==XI_ButtonPress)pointer({PointerAction::wheel,at,event.detail==Button4?1.:-1.,fine},event.mods.effective);return;}
        if(event.detail==Button3){if(type==XI_ButtonPress){focusIfViewable();pointer({PointerAction::context,at,0,fine},event.mods.effective);}return;}
        if(event.detail!=Button1&&event.detail!=Button2)return;const bool down=type==XI_ButtonPress,reset=down&&event.time-lastClick<300&&std::abs(at.x-lastClickX)<5&&std::abs(at.y-lastClickY)<5;
        if(down){lastClick=event.time;lastClickX=int(at.x);lastClickY=int(at.y);focusIfViewable();}if(down){unsigned char mask[XIMaskLen(XI_LASTEVENT)]{};for(int kind:{XI_ButtonPress,XI_ButtonRelease,XI_Motion})XISetMask(mask,kind);XIEventMask events{event.deviceid,int(sizeof(mask)),mask};if(checked::xiGrab(display,event.deviceid,handle,event.time,{mask,sizeof(mask)})==GrabSuccess)pointerDevice=event.deviceid;}else releasePointer();PointerEvent out{down?PointerAction::down:PointerAction::up,at,0,fine,reset};out.button=event.detail==Button2?PointerButton::middle:PointerButton::primary;pointer(out,event.mods.effective);
    }
#endif
    void paint() {
        surface.render([&](Canvas& c){c.scale(contentScale,contentScale);editor.paint(c,{0,0,surface.width()/contentScale,surface.height()/contentScale});});
        if(!alive||closePending)return;
        // Embedded children inherit their parent's visual/depth. A plugin host
        // can use ARGB32 even when the desktop root is RGB24.
        auto* image=XCreateImage(display,visual,unsigned(depth),ZPixmap,0,
            static_cast<char*>(const_cast<void*>(surface.pixels())),unsigned(surface.width()),unsigned(surface.height()),32,int(surface.stride()));
        if(image) {
            struct Release{XImage*image;~Release(){image->data=nullptr;XDestroyImage(image);}}release{image};
            const int nativeOrder=std::endian::native==std::endian::little?LSBFirst:MSBFirst;
            const bool direct=image->bits_per_pixel==32&&image->byte_order==nativeOrder&&
                visual->red_mask==0xff0000&&visual->green_mask==0xff00&&visual->blue_mask==0xff;
            if(!direct){
                convertedPixels.resize(std::size_t(image->bytes_per_line)*std::size_t(surface.height()));
                image->data=reinterpret_cast<char*>(convertedPixels.data());
                const auto channel=[](unsigned value,unsigned long mask){
                    if(!mask)return 0UL;
                    const auto shift=std::countr_zero(mask);const auto maximum=mask>>shift;
                    return static_cast<unsigned long>((std::uint64_t(value)*maximum+127)/255)<<shift;
                };
                for(int y=0;y<surface.height();++y){
                    const auto*row=reinterpret_cast<const std::uint32_t*>(static_cast<const unsigned char*>(surface.pixels())+std::size_t(y)*surface.stride());
                    for(int x=0;x<surface.width();++x){const auto pixel=row[x];XPutPixel(image,x,y,channel((pixel>>16)&255,visual->red_mask)|channel((pixel>>8)&255,visual->green_mask)|channel(pixel&255,visual->blue_mask));}
                }
            }
            if(!checked::putImage(display,handle,gc,image)){closePending=true;return;}
        }
        if(!alive||closePending)return;XFlush(display);dirty=false;
    }
};
Window::Window(Editor& e):impl_(std::make_shared<Impl>(e)){}
Window::~Window(){impl_->editor.nativeWindowHandle=0;impl_->alive=false;impl_->editor.invalidate={};impl_->close();}
bool Window::open(std::uintptr_t parent,int width,int height) {
    auto lifetime=impl_;auto& p=*lifetime;if(p.handle||width<1||height<1||width>8192||height>8192)return false;
    p.display=XOpenDisplay(nullptr);if(!p.display)return false;
    p.embedParent=::Window(parent);p.forwardedKeys.fill(false);p.resizeRequestSerial=0;p.resizePending=false;p.handle=checked::createWindow(p.display,parent?::Window(parent):DefaultRootWindow(p.display),unsigned(width),unsigned(height));
    if(!p.handle){p.close();return false;}
    XWindowAttributes attributes{};
    if(!::cell::ui::checked::attributes(p.display,p.handle,&attributes)||!attributes.visual||attributes.visual->c_class!=TrueColor){p.close();return false;}
    p.visual=attributes.visual;p.depth=attributes.depth;
    p.gc=checked::createGC(p.display,p.handle);
    #if CELL_HAS_XI2
    p.xi2=p.selectTouch();
#endif
    ::cell::ui::checked::selectInput(p.display,p.handle,ExposureMask|StructureNotifyMask|KeyPressMask|KeyReleaseMask|FocusChangeMask|(p.xi2?0:ButtonPressMask|ButtonReleaseMask|PointerMotionMask));
    Bool repeatSupported{};XkbSetDetectableAutoRepeat(p.display,True,&repeatSupported);
    p.closeAtom=XInternAtom(p.display,"WM_DELETE_WINDOW",False);::cell::ui::checked::wmProtocols(p.display,p.handle,&p.closeAtom,1);
    native::registerDropTarget(p.display,p.handle,p.editor);p.editor.nativeWindowHandle=p.handle;::cell::ui::checked::storeName(p.display,p.handle,p.editor.title());p.surface.resize(width,height);p.dirty=true;
    return true;
}
void Window::close(){auto lifetime=impl_;if(lifetime->alive)lifetime->editor.visibility(false);lifetime->close();}
void Window::setPosition(int x,int y){auto lifetime=impl_;auto&p=*lifetime;if(p.display&&p.handle)::cell::ui::checked::moveWindow(p.display,p.handle,x,y);}
void Window::setSizeLimits(int minW,int minH,int maxW,int maxH){auto lifetime=impl_;auto&p=*lifetime;if(!p.display||!p.handle)return;XSizeHints hints{};hints.flags=PMinSize|PMaxSize;hints.min_width=std::max(1,minW);hints.min_height=std::max(1,minH);hints.max_width=std::max(hints.min_width,maxW);hints.max_height=std::max(hints.min_height,maxH);::cell::ui::checked::sizeHints(p.display,p.handle,&hints);}
void Window::show(bool visible) {
    auto lifetime=impl_;auto& p=*lifetime;if(!p.handle)return;
    p.requestedVisible=visible;
    if(visible)::cell::ui::checked::mapWindow(p.display,p.handle);
    else if(p.embedParent)::cell::ui::checked::unmapWindow(p.display,p.handle);
    // Managed top-level windows must also notify the window manager when the
    // client withdraws. A bare unmap can lose a rapid reopen while the first
    // MapRequest is still waiting in the manager's event queue.
    else ::cell::ui::checked::withdrawWindow(p.display,p.handle,DefaultScreen(p.display));
    if(!visible){p.visible=false;p.focusRequestUntil={};}p.dirty=true;XFlush(p.display);
    p.editor.visibility(visible);
}
bool Window::resize(int width,int height) {
    auto lifetime=impl_;auto& p=*lifetime;if(!p.handle||width<1||height<1||width>8192||height>8192)return false;
    // Layout can reflect a native resize back through DocumentWindow. An
    // unchanged size needs neither another server request nor a raster change.
    if(p.surface.width()==width&&p.surface.height()==height)return true;
    p.resizeRequestSerial=checked::resizeWindow(p.display,p.handle,unsigned(width),unsigned(height));if(!p.resizeRequestSerial)return false;
    p.resizePending=true;p.resizeDeadline=std::chrono::steady_clock::now()+std::chrono::milliseconds(250);
    p.surface.resize(width,height);p.dirty=true;return true;
}
void Window::poll() {
    auto lifetime=impl_;auto& p=*lifetime;if(!p.handle||!p.alive)return;
    native::pollTransfers();
    ++p.dispatchDepth;struct Dispatch{Impl&p;~Dispatch(){if(--p.dispatchDepth==0&&p.closePending)p.close();}}dispatch{p};
    while(p.handle&&p.alive&&!p.closePending&&XPending(p.display)) {
        XEvent event{};XNextEvent(p.display,&event);
        if(event.type==DestroyNotify&&event.xdestroywindow.window==p.handle){native::unregisterDropTarget(p.handle);p.handle=0;p.closePending=true;p.editor.visibility(false);return;}
        if(native::handleDropEvent(p.display,event,p.editor))continue;
#if CELL_HAS_XI2
        if(p.xi2&&event.type==GenericEvent&&event.xcookie.extension==p.xiOpcode&&XGetEventData(p.display,&event.xcookie)){
            const int type=event.xcookie.evtype;if(type==XI_TouchBegin||type==XI_TouchUpdate||type==XI_TouchEnd||type==XI_ButtonPress||type==XI_ButtonRelease||type==XI_Motion)p.xiPointer(*static_cast<XIDeviceEvent*>(event.xcookie.data),type);
            XFreeEventData(p.display,&event.xcookie);continue;
        }
#endif
        if(event.type==Expose)p.dirty=true;
        else if(event.type==FocusOut){
            // Moving explicit focus from the host into its pointer-focused
            // child can enqueue NotifyPointer FocusOut immediately before
            // FocusIn. Do not discard the click's edit when the server now
            // explicitly focuses this window. Real losses and keyboard grabs
            // still cancel input, including losses queued before refocusing.
            ::Window focused{};int revert{};
            const bool normal=event.xfocus.mode==NotifyNormal;
            const bool known=normal&&(event.xfocus.detail==NotifyPointer||event.xfocus.detail==NotifyAncestor)&&XGetInputFocus(p.display,&focused,&revert);
            const bool pointerEntry=known&&event.xfocus.detail==NotifyPointer&&focused==p.handle;
            if(!pointerEntry){
                // Some hosts activate their containing frame just after our
                // first pointer focus request. Complete that handoff once,
                // only while the pointer is really in our embedded subtree.
                const bool handoff=known&&event.xfocus.detail==NotifyAncestor&&p.visible&&p.requestedVisible&&
                    std::chrono::steady_clock::now()<p.focusRequestUntil&&p.pointerFocusInHostAncestor(focused);
                p.focusRequestUntil={};
                if(handoff)::cell::ui::checked::setInputFocus(p.display,p.handle,RevertToParent,CurrentTime);
                else {p.releasePointer();p.editor.cancelInput();}
            }
        }
        else if(event.type==MapNotify){
            p.visible=p.requestedVisible;p.editor.visibility(p.visible);p.dirty=true;
            if(!p.requestedVisible){if(p.embedParent)::cell::ui::checked::unmapWindow(p.display,p.handle);else ::cell::ui::checked::withdrawWindow(p.display,p.handle,DefaultScreen(p.display));XFlush(p.display);}
        }
        else if(event.type==UnmapNotify){p.visible=false;p.focusRequestUntil={};p.editor.visibility(false);}
        else if(event.type==ConfigureNotify) {
            const auto& e=event.xconfigure;
            if(e.window!=p.handle)continue;
            // A queued reply from an earlier resize must not replace the
            // surface geometry the host has already requested more recently.
            if(p.resizeRequestSerial&&event.xany.serial<p.resizeRequestSerial)continue;
            // A manager's reply to an older ConfigureRequest may carry a newer
            // client sequence number: its processing happens on another X11
            // connection. Keep the newest logical size until its actual reply.
            // A denied/constrained request is reconciled below after a short
            // asynchronous acknowledgement interval; the GUI never blocks.
            if(p.resizePending){
                if(e.width==p.surface.width()&&e.height==p.surface.height())p.resizePending=false;
                else continue;
            }
            if(e.width!=p.surface.width()||e.height!=p.surface.height()){
                // Notifications are snapshots. Coalesce them to the server's
                // actual geometry instead of painting an obsolete snapshot.
                XWindowAttributes attributes{};
                if(::cell::ui::checked::attributes(p.display,p.handle,&attributes)&&attributes.width>0&&attributes.height>0&&
                   attributes.width<=8192&&attributes.height<=8192&&
                   (attributes.width!=p.surface.width()||attributes.height!=p.surface.height())){
                    p.editor.cancelInput();p.surface.resize(attributes.width,attributes.height);p.dirty=true;
                }
            }
        } else if(event.type==ClientMessage&&Atom(event.xclient.data.l[0])==p.closeAtom) {p.editor.visibility(false);p.close();return;}
        else if(event.type==DestroyNotify){p.editor.visibility(false);p.visible=false;p.handle=0;return;}
        else if(event.type==KeyPress||event.type==KeyRelease) {
            if(event.type==KeyPress)p.focusRequestUntil={};
            if(event.type==KeyRelease&&XPending(p.display)){XEvent next{};XPeekEvent(p.display,&next);if(next.type==KeyPress&&next.xkey.keycode==event.xkey.keycode&&next.xkey.time==event.xkey.time)continue;}
            const auto key=XLookupKeysym(&event.xkey,(event.xkey.state&ShiftMask)?1:0);
            unsigned keyModifiers=event.xkey.state;if(event.type==KeyRelease){if(key==XK_Shift_L||key==XK_Shift_R)keyModifiers&=~ShiftMask;if(key==XK_Control_L||key==XK_Control_R)keyModifiers&=~ControlMask;if(key==XK_Alt_L||key==XK_Alt_R)keyModifiers&=~Mod1Mask;if(key==XK_Super_L||key==XK_Super_R)keyModifiers&=~Mod4Mask;}const bool shift=bool(keyModifiers&ShiftMask);const int code=key==XK_Tab||key==XK_ISO_Left_Tab?9:key==XK_Up?keys::up:key==XK_Down?keys::down:key==XK_Left?keys::left:key==XK_Right?keys::right:key==XK_Home?keys::home:key==XK_End?keys::end:key==XK_Menu||(key==XK_F10&&shift)?keys::contextMenu:key==XK_Return||key==XK_KP_Enter?13:key==XK_Escape?27:key==XK_BackSpace?8:key==XK_Delete?127:key==XK_Page_Up?1010:key==XK_Page_Down?1011:key>=XK_F1&&key<=XK_F12?1101+int(key-XK_F1):int(key);
            const KeyEvent keyEvent{code,shift,bool(keyModifiers&ControlMask),bool(keyModifiers&Mod1Mask),bool(keyModifiers&Mod4Mask),false,code>=32&&code<127?char32_t(code):0};
            const auto keycode=std::min(unsigned(event.xkey.keycode),255u);
            if(event.type==KeyRelease){const bool handled=p.editor.keyReleased(keyEvent);if(!p.alive||p.closePending)return;if(p.forwardedKeys[keycode]){p.forwardedKeys[keycode]=false;p.forwardKey(event);}else(void)handled;}
            else if(!p.editor.keyboard(keyEvent)){if(!p.alive||p.closePending)return;p.forwardedKeys[keycode]=true;p.forwardKey(event);}
        } else if(event.type==MotionNotify) {
            p.pointer({PointerAction::move,{double(event.xmotion.x),double(event.xmotion.y)},0,bool(event.xmotion.state&ShiftMask),false},event.xmotion.state);
        } else if(event.type==ButtonPress||event.type==ButtonRelease) {
            const auto& e=event.xbutton;
            if(e.button==Button4||e.button==Button5) {
                if(event.type==ButtonPress)p.pointer({PointerAction::wheel,{double(e.x),double(e.y)},e.button==Button4?1.0:-1.0,bool(e.state&ShiftMask),false},e.state);
            } else if(e.button==Button3&&event.type==ButtonPress) {
                p.focusIfViewable();
                p.pointer({PointerAction::context,{double(e.x),double(e.y)},0,bool(e.state&ShiftMask)},e.state);
            } else if(e.button==Button1||e.button==Button2) {
                const bool down=event.type==ButtonPress;
                const bool reset=down&&e.time-p.lastClick<300&&std::abs(e.x-p.lastClickX)<5&&std::abs(e.y-p.lastClickY)<5;
                if(down){p.lastClick=e.time;p.lastClickX=e.x;p.lastClickY=e.y;p.focusIfViewable();}
                if(down)p.pointerGrabbed=::cell::ui::checked::grabPointer(p.display,p.handle,False,ButtonPressMask|ButtonReleaseMask|PointerMotionMask,GrabModeAsync,GrabModeAsync,None,None,e.time)==GrabSuccess;else p.releasePointer();PointerEvent out{down?PointerAction::down:PointerAction::up,{double(e.x),double(e.y)},0,bool(e.state&ShiftMask),reset};out.button=e.button==Button2?PointerButton::middle:PointerButton::primary;p.pointer(out,e.state);
            }
        }
    }
    if(p.handle&&p.alive&&!p.closePending&&p.resizePending&&std::chrono::steady_clock::now()>=p.resizeDeadline){
        XWindowAttributes attributes{};
        if(::cell::ui::checked::attributes(p.display,p.handle,&attributes)){
            p.resizePending=false;
            if(attributes.width>0&&attributes.height>0&&attributes.width<=8192&&attributes.height<=8192&&
               (attributes.width!=p.surface.width()||attributes.height!=p.surface.height())){
                p.editor.cancelInput();p.surface.resize(attributes.width,attributes.height);p.dirty=true;
            }
        }
    }
    // The window manager may finish an older withdrawal after a newer show
    // request. Reconcile the requested state with the server until mapping is
    // confirmed; steady visible windows incur no extra query or remap.
    if(p.handle&&p.alive&&!p.closePending&&p.requestedVisible&&!p.visible){
        XWindowAttributes attributes{};
        if(::cell::ui::checked::attributes(p.display,p.handle,&attributes)){
            if(attributes.map_state==IsUnmapped){::cell::ui::checked::mapWindow(p.display,p.handle);XFlush(p.display);}
            else if(attributes.map_state==IsViewable){p.visible=true;p.editor.visibility(true);p.dirty=true;}
        }
    }
    // Data synchronization runs even when native exposes/input already marked
    // the surface dirty. Short-circuiting here freezes data during interaction.
    if(p.visible&&p.alive&&!p.closePending){p.updateCursor();const bool changed=p.editor.advanceFrame();if(p.alive&&!p.closePending&&(p.dirty||changed))p.paint();}
}
bool Window::isOpen()const{return impl_->handle!=0;}
std::uintptr_t Window::nativeHandle()const{return impl_->handle;}
bool Window::setContentScaleFactor(double factor){
    auto lifetime=impl_;auto& p=*lifetime;if(!validContentScale(factor))return false;if(factor==p.contentScale)return true;
    p.editor.cancelInput();p.contentScale=factor;p.editor.nativeWindowScale=factor;p.dirty=true;return true;
}
double Window::contentScaleFactor()const noexcept{return impl_->contentScale;}
Point Window::toNativePoint(Point point)const{return nativePoint(point,impl_->contentScale);}
}

namespace cell::ui {bool Window::beginFileDrag(std::span<const std::string>files,bool move){auto lifetime=impl_;auto&p=*lifetime;if(!p.handle||!p.alive)return false;p.releasePointer();if(native::beginFileDrag(p.handle,files,move)){p.editor.cancelInput();return true;}return false;}}
