#include <cell/ui.hpp>
#include <cell/ui_scaling.hpp>
#include "platform_transfer.hpp"
#ifndef _WIN32_WINNT
#define _WIN32_WINNT 0x0602
#endif
#include <windows.h>
#include <windowsx.h>
#include <climits>

namespace cell::ui {
struct Window::Impl:std::enable_shared_from_this<Window::Impl> {
    Editor& editor;Surface surface;HWND handle{};bool dirty=true;
    explicit Impl(Editor& e):editor(e){editor.invalidate=[this]{dirty=true;if(handle)InvalidateRect(handle,nullptr,FALSE);};}
    void*dropTarget{};char16_t highSurrogate{};bool embedded{},ownerAlive=true,mouseDown{};
    double contentScale=1;int minWidth=1,minHeight=1,maxWidth=INT_MAX,maxHeight=INT_MAX;
    static double monitorScale(HWND window){using Query=UINT(WINAPI*)(HWND);auto query=reinterpret_cast<Query>(GetProcAddress(GetModuleHandleW(L"user32.dll"),"GetDpiForWindow"));const auto dpi=query?query(window):0;return dpi?double(dpi)/96.:1.;}
    std::array<bool,256> forwardedDown{},forwardedChar{},handledDown{};
    void updateCursor(){const int type=editor.pointerCursor();const auto id=type==2?IDC_WAIT:type==3?IDC_IBEAM:type==4||type==5?IDC_CROSS:type==6?IDC_HAND:type==8?IDC_SIZEWE:type==9?IDC_SIZENS:type==7||type>=10?IDC_SIZEALL:IDC_ARROW;SetCursor(LoadCursorW(nullptr,reinterpret_cast<LPCWSTR>(id)));}
    void forwardKey(UINT message,WPARAM key,LPARAM flags){if(auto parent=GetParent(handle))PostMessageW(parent,message,key,flags);}
    void pointer(PointerEvent event,std::optional<WPARAM> nativeKeys={}){
        event.position=logicalPoint(event.position,contentScale);
        // Sample the complete native modifier state for every pointer action.
        // WidgetHost replaces its modifier snapshot from this event: carrying
        // only Shift silently turns Ctrl/Alt wheel zoom into ordinary scrolling.
        // Wheel messages carry the modifier state at generation time. Hosts may
        // forward them after changing their keyboard queue state.
        event.fine=nativeKeys?(*nativeKeys&MK_SHIFT)!=0:(GetKeyState(VK_SHIFT)&0x8000)!=0;
        event.control=nativeKeys?(*nativeKeys&MK_CONTROL)!=0:(GetKeyState(VK_CONTROL)&0x8000)!=0;
        event.alt=(GetKeyState(VK_MENU)&0x8000)!=0;
        editor.pointer(event);
        if(ownerAlive&&handle&&event.identity.kind==PointerKind::mouse)updateCursor();
    }
    ~Impl(){native::unregisterDropTarget(dropTarget);dropTarget=nullptr;if(ownerAlive){editor.invalidate={};editor.visibility(false);}if(handle){SetWindowLongPtrW(handle,GWLP_USERDATA,0);DestroyWindow(handle);}}
    void paint(HDC dc) {
        const bool changed=editor.advanceFrame();
        if(!ownerAlive||!handle)return;
        if(dirty||changed) {
            surface.render([&](Canvas& c){c.scale(contentScale,contentScale);editor.paint(c,{0,0,surface.width()/contentScale,surface.height()/contentScale});});dirty=false;
        }
        if(!ownerAlive||!handle)return;
        BITMAPINFO info{};info.bmiHeader.biSize=sizeof(BITMAPINFOHEADER);
        // Blend2D aligns scanlines independently of the visible width. GDI
        // derives row pitch from biWidth: describing only visible pixels
        // makes every subsequent row start inside the preceding row's padding.
        // Describe the actual storage width, then blit only the visible area.
        const auto stride=surface.stride();
        if(stride<=0||stride%4||stride/4>LONG_MAX)return;
        info.bmiHeader.biWidth=LONG(stride/4);info.bmiHeader.biHeight=-surface.height();
        info.bmiHeader.biPlanes=1;info.bmiHeader.biBitCount=32;info.bmiHeader.biCompression=BI_RGB;
        SetDIBitsToDevice(dc,0,0,DWORD(surface.width()),DWORD(surface.height()),0,0,0,UINT(surface.height()),surface.pixels(),&info,DIB_RGB_COLORS);
    }
    static bool promotedMouse()noexcept{
#if _WIN32_WINNT >= 0x0602
        return (std::uintptr_t(GetMessageExtraInfo())&0xffffff00u)==0xff515700u;
#else
        return false;
#endif
    }
    static KeyEvent keyEvent(int key)noexcept{return {key,(GetKeyState(VK_SHIFT)&0x8000)!=0,(GetKeyState(VK_CONTROL)&0x8000)!=0,(GetKeyState(VK_MENU)&0x8000)!=0,false,false};}
    static LRESULT CALLBACK proc(HWND window,UINT message,WPARAM w,LPARAM l) {
        auto* self=reinterpret_cast<Impl*>(GetWindowLongPtrW(window,GWLP_USERDATA));
        if(message==WM_NCCREATE) {
            self=static_cast<Impl*>(reinterpret_cast<CREATESTRUCTW*>(l)->lpCreateParams);
            SetWindowLongPtrW(window,GWLP_USERDATA,reinterpret_cast<LONG_PTR>(self));self->handle=window;self->editor.nativeWindowHandle=reinterpret_cast<std::uintptr_t>(window);
        }
        if(!self||!self->ownerAlive)return DefWindowProcW(window,message,w,l);
        auto lifetime=self->weak_from_this().lock();
        switch(message) {
        case WM_PAINT:{PAINTSTRUCT paint{};HDC dc=BeginPaint(window,&paint);self->paint(dc);EndPaint(window,&paint);return 0;}
        case WM_ERASEBKGND:return 1;
        case WM_SETCURSOR:if(LOWORD(l)==HTCLIENT){self->updateCursor();return TRUE;}break;
        case WM_SIZE:{self->editor.cancelInput();if(!self->ownerAlive)return 0;const int width=LOWORD(l),height=HIWORD(l);if(width>0&&height>0&&width<=8192&&height<=8192){self->surface.resize(width,height);self->dirty=true;}return 0;}
        case WM_TIMER:{if(!IsWindowVisible(window))return 0;const bool changed=self->editor.advanceFrame();if(self->ownerAlive&&self->handle&&(changed||self->dirty))InvalidateRect(window,nullptr,FALSE);return 0;}
        case WM_LBUTTONDOWN:case WM_LBUTTONDBLCLK:
            if(promotedMouse())return 0;self->mouseDown=true;SetFocus(window);SetCapture(window);
            self->pointer({PointerAction::down,{double(GET_X_LPARAM(l)),double(GET_Y_LPARAM(l))},0,(GetKeyState(VK_SHIFT)&0x8000)!=0,message==WM_LBUTTONDBLCLK});return 0;
        case WM_RBUTTONDOWN:if(promotedMouse())return 0;SetFocus(window);self->pointer({PointerAction::context,{double(GET_X_LPARAM(l)),double(GET_Y_LPARAM(l))}});return 0;
        case WM_MOUSEMOVE:if(promotedMouse())return 0;self->pointer({PointerAction::move,{double(GET_X_LPARAM(l)),double(GET_Y_LPARAM(l))},0,(GetKeyState(VK_SHIFT)&0x8000)!=0,false});return 0;
        case WM_LBUTTONUP:if(promotedMouse())return 0;self->mouseDown=false;self->pointer({PointerAction::up,{double(GET_X_LPARAM(l)),double(GET_Y_LPARAM(l))},0,(GetKeyState(VK_SHIFT)&0x8000)!=0,false});if(GetCapture()==window)ReleaseCapture();return 0;
        case WM_MOUSEWHEEL:{POINT point{GET_X_LPARAM(l),GET_Y_LPARAM(l)};ScreenToClient(window,&point);self->pointer({PointerAction::wheel,{double(point.x),double(point.y)},double(GET_WHEEL_DELTA_WPARAM(w))/WHEEL_DELTA,(GetKeyState(VK_SHIFT)&0x8000)!=0,false},GET_KEYSTATE_WPARAM(w));return 0;}
        case WM_DPICHANGED:if(!self->embedded){self->contentScale=double(HIWORD(w))/96.;self->editor.nativeWindowScale=self->contentScale;self->editor.cancelInput();const auto*next=reinterpret_cast<const RECT*>(l);SetWindowPos(window,nullptr,next->left,next->top,next->right-next->left,next->bottom-next->top,SWP_NOZORDER|SWP_NOACTIVATE);self->dirty=true;return 0;}break;
        case WM_GETMINMAXINFO:{
            auto*info=reinterpret_cast<MINMAXINFO*>(l);RECT frame{0,0,0,0};
            const auto style=DWORD(GetWindowLongPtrW(window,GWL_STYLE));
            if(!(style&WS_CHILD))AdjustWindowRectEx(&frame,style,FALSE,DWORD(GetWindowLongPtrW(window,GWL_EXSTYLE)));
            const auto width=std::int64_t(frame.right)-frame.left,height=std::int64_t(frame.bottom)-frame.top;
            const auto limit=[](int size,std::int64_t border){return LONG(std::clamp(std::int64_t(size)+border,std::int64_t(1),std::int64_t(LONG_MAX)));};
            info->ptMinTrackSize={limit(self->minWidth,width),limit(self->minHeight,height)};
            info->ptMaxTrackSize={limit(self->maxWidth,width),limit(self->maxHeight,height)};return 0;
        }
        case WM_GETDLGCODE:return DLGC_WANTARROWS|DLGC_WANTTAB|DLGC_WANTCHARS;
        case WM_KEYDOWN:{
            // A bare modifier has no host action. Forwarding Control itself
            // makes some hosts reset its state before the wheel arrives.
            // Chord keys still follow the normal handled/forwarded path below.
            if(w==VK_CONTROL||w==VK_SHIFT||w==VK_MENU)break;
            const bool shift=(GetKeyState(VK_SHIFT)&0x8000)!=0;const int key=w==VK_ESCAPE?27:w==VK_TAB?9:w==VK_UP?keys::up:w==VK_DOWN?keys::down:w==VK_LEFT?keys::left:w==VK_RIGHT?keys::right:w==VK_DELETE?keys::deleteKey:w==VK_HOME?keys::home:w==VK_END?keys::end:w==VK_APPS||(w==VK_F10&&shift)?keys::contextMenu:0;
            auto event=keyEvent(key?key:int(w));event.repeat=(l&(1ll<<30))!=0;if(!event.repeat&&w<256)self->forwardedDown[w]=self->forwardedChar[w]=self->handledDown[w]=false;
            if(key||event.control||event.alt){const bool handled=self->editor.keyboard(event);if(w<256){self->handledDown[w]=handled;self->forwardedDown[w]=!handled;}if(!handled)self->forwardKey(WM_KEYDOWN,w,l);return 0;}break;
        }
        case WM_KEYUP:{const bool handled=self->editor.keyReleased(keyEvent(int(w)));if(w<256){if(self->forwardedDown[w]||self->forwardedChar[w])self->forwardKey(WM_KEYUP,w,l);self->forwardedDown[w]=self->forwardedChar[w]=self->handledDown[w]=false;}if(handled)return 0;break;}
        case WM_CHAR:{
            const auto scan=UINT((l>>16)&0xff)|((l&(1ll<<24))?0xe000u:0u);const auto key=MapVirtualKeyW(scan,MAPVK_VSC_TO_VK_EX);
            if(key<256&&self->handledDown[key])return 0;
            char32_t point=char32_t(w);
            if(point>=0xd800&&point<=0xdbff){self->highSurrogate=char16_t(point);return 0;}
            if(point>=0xdc00&&point<=0xdfff){if(!self->highSurrogate)return 0;point=0x10000+(char32_t(self->highSurrogate)-0xd800)*0x400+(point-0xdc00);}
            self->highSurrogate=0;auto event=keyEvent(point<128?int(point):((key>='A'&&key<='Z')||(key>='0'&&key<='9')?int(key):0));event.text=point;
            if(point!=9&&self->editor.keyboard(event))return 0;
            if(key&&key<256&&!self->forwardedDown[key]){self->forwardedChar[key]=true;self->forwardKey(WM_KEYDOWN,key,l);}return 0;
        }
        case WM_UNICHAR:{if(w==UNICODE_NOCHAR)return TRUE;auto event=keyEvent(int(w));event.text=char32_t(w);return self->editor.keyboard(event)?0:DefWindowProcW(window,message,w,l);}
        case WM_KILLFOCUS:case WM_CANCELMODE:self->mouseDown=false;self->highSurrogate=0;self->editor.cancelInput();return 0;
        // A normal mouse-up already ended its gesture. ReleaseCapture sends this
        // message too; treating it as cancellation clears keyboard focus after
        // every click. Only an unfinished press needs capture-loss cancellation.
        case WM_CAPTURECHANGED:if(self->mouseDown&&reinterpret_cast<HWND>(l)!=window){self->mouseDown=false;self->pointer({PointerAction::cancel});}return 0;
#if _WIN32_WINNT >= 0x0602
        case WM_POINTERDOWN:case WM_POINTERUPDATE:case WM_POINTERUP:{
            const UINT32 id=GET_POINTERID_WPARAM(w);POINTER_INPUT_TYPE kind{};if(!GetPointerType(id,&kind)||kind==PT_MOUSE)break;
            if(kind!=PT_TOUCH&&kind!=PT_PEN)break;const PointerIdentity identity{id,kind==PT_TOUCH?PointerKind::touch:PointerKind::pen};POINTER_INFO info{};
            if(!GetPointerInfo(id,&info)){self->pointer({PointerAction::cancel,{},0,false,false,identity});return 0;}
            POINT point=info.ptPixelLocation;ScreenToClient(window,&point);if(message==WM_POINTERDOWN)SetFocus(window);
            const auto action=(info.pointerFlags&POINTER_FLAG_CANCELED)?PointerAction::cancel:message==WM_POINTERDOWN?PointerAction::down:message==WM_POINTERUP?PointerAction::up:PointerAction::move;
            self->pointer({action,{double(point.x),double(point.y)},0,(GetKeyState(VK_SHIFT)&0x8000)!=0,false,identity});return 0;
        }
        case WM_POINTERCAPTURECHANGED:{const auto id=GET_POINTERID_WPARAM(w);for(auto kind:{PointerKind::touch,PointerKind::pen})self->pointer({PointerAction::cancel,{},0,false,false,{id,kind}});return 0;}
#endif
        case WM_CLOSE:DestroyWindow(window);return 0;
        case WM_DESTROY:SetWindowLongPtrW(window,GWLP_USERDATA,0);self->editor.nativeWindowHandle=0;native::unregisterDropTarget(self->dropTarget);self->dropTarget=nullptr;self->editor.visibility(false);KillTimer(window,1);self->handle=nullptr;return 0;
        }
        return DefWindowProcW(window,message,w,l);
    }
    struct Registration {
        HINSTANCE module{};
        Registration(){
            GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS|GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                reinterpret_cast<LPCWSTR>(&Impl::proc),&module);
            WNDCLASSW cls{};cls.style=CS_DBLCLKS;cls.lpfnWndProc=Impl::proc;cls.hInstance=module;
            cls.hCursor=LoadCursorW(nullptr,reinterpret_cast<LPCWSTR>(IDC_ARROW));cls.lpszClassName=L"CellFrameworkNativeEditor01";RegisterClassW(&cls);
        }
        ~Registration(){UnregisterClassW(L"CellFrameworkNativeEditor01",module);}
    };
    static Registration& registration(){static Registration value;return value;}
};
Window::Window(Editor& e):impl_(std::make_shared<Impl>(e)){}
Window::~Window(){auto lifetime=std::move(impl_);if(!lifetime)return;lifetime->ownerAlive=false;lifetime->editor.invalidate={};lifetime->editor.nativeWindowHandle=0;native::unregisterDropTarget(lifetime->dropTarget);lifetime->dropTarget=nullptr;if(lifetime->handle){const auto window=lifetime->handle;lifetime->handle=nullptr;SetWindowLongPtrW(window,GWLP_USERDATA,0);KillTimer(window,1);DestroyWindow(window);}}
bool Window::open(std::uintptr_t parent,int width,int height){
    auto lifetime=impl_;auto& p=*lifetime;if(p.handle||width<1||height<1||width>8192||height>8192)return false;
    p.embedded=parent!=0;p.surface.resize(width,height);const DWORD style=parent?WS_CHILD:WS_OVERLAPPEDWINDOW;
    RECT r{0,0,width,height};if(!parent)AdjustWindowRect(&r,style,FALSE);
    const auto* utf8=p.editor.title();const auto length=MultiByteToWideChar(CP_UTF8,0,utf8,-1,nullptr,0);
    std::wstring title(std::size_t(std::max(1,length)),L'\0');MultiByteToWideChar(CP_UTF8,0,utf8,-1,title.data(),length);
    p.handle=CreateWindowExW(0,L"CellFrameworkNativeEditor01",title.c_str(),style,
        parent?0:CW_USEDEFAULT,parent?0:CW_USEDEFAULT,r.right-r.left,r.bottom-r.top,
        reinterpret_cast<HWND>(parent),nullptr,Impl::registration().module,&p);
    if(!p.ownerAlive)return false;
    if(p.handle&&!parent){const auto scale=Impl::monitorScale(p.handle);if(scale!=p.contentScale){p.contentScale=scale;p.editor.nativeWindowScale=scale;resize(int(std::lround(width*scale)),int(std::lround(height*scale)));}}
    if(!p.ownerAlive)return false;
    if(p.handle)p.dropTarget=native::registerDropTarget(reinterpret_cast<std::uintptr_t>(p.handle),p.editor);
    return p.handle!=nullptr;
}
void Window::close(){auto lifetime=impl_;lifetime->editor.nativeWindowHandle=0;lifetime->editor.visibility(false);if(lifetime->handle)DestroyWindow(lifetime->handle);}
void Window::setPosition(int x,int y){auto&p=*impl_;if(p.handle)SetWindowPos(p.handle,nullptr,x,y,0,0,SWP_NOSIZE|SWP_NOZORDER|SWP_NOACTIVATE);}
void Window::setSizeLimits(int a,int b,int c,int d){auto&p=*impl_;p.minWidth=std::max(1,a);p.minHeight=std::max(1,b);p.maxWidth=std::max(p.minWidth,c);p.maxHeight=std::max(p.minHeight,d);}
void Window::show(bool visible){auto lifetime=impl_;auto& p=*lifetime;if(!p.handle)return;ShowWindow(p.handle,visible?SW_SHOW:SW_HIDE);if(!p.ownerAlive||!p.handle)return;
    p.editor.visibility(visible);if(!p.ownerAlive||!p.handle)return;
    if(visible)SetTimer(p.handle,1,33,nullptr);else KillTimer(p.handle,1);}
bool Window::resize(int width,int height){auto& p=*impl_;if(!p.handle||width<1||height<1||width>8192||height>8192)return false;
    RECT rect{0,0,width,height};AdjustWindowRect(&rect,DWORD(GetWindowLongPtrW(p.handle,GWL_STYLE)),FALSE);
    return SetWindowPos(p.handle,nullptr,0,0,rect.right-rect.left,rect.bottom-rect.top,SWP_NOMOVE|SWP_NOZORDER)!=0;}
void Window::poll(){auto lifetime=impl_;MSG message{};while(lifetime->ownerAlive&&lifetime->handle&&PeekMessageW(&message,lifetime->handle,0,0,PM_REMOVE)){TranslateMessage(&message);DispatchMessageW(&message);}}
bool Window::isOpen()const{return impl_->handle!=nullptr;}
std::uintptr_t Window::nativeHandle()const{return reinterpret_cast<std::uintptr_t>(impl_->handle);}
bool Window::setContentScaleFactor(double factor){
    auto lifetime=impl_;auto& p=*lifetime;if(!validContentScale(factor))return false;if(factor==p.contentScale)return true;
    p.editor.cancelInput();if(!p.ownerAlive)return false;p.contentScale=factor;p.editor.nativeWindowScale=factor;p.dirty=true;if(p.handle)InvalidateRect(p.handle,nullptr,FALSE);return true;
}
double Window::contentScaleFactor()const noexcept{return impl_->contentScale;}
Point Window::toNativePoint(Point point)const{return nativePoint(point,impl_->contentScale);}
}

namespace cell::ui {bool Window::beginFileDrag(std::span<const std::string>files,bool move){auto lifetime=impl_;if(!lifetime->handle)return false;ReleaseCapture();lifetime->editor.cancelInput();return native::beginFileDrag(reinterpret_cast<std::uintptr_t>(lifetime->handle),files,move);}}
