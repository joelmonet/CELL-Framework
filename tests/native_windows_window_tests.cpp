// Original Cell Framework implementation; see LICENSE.
#include <cell/ui.hpp>
#include <cell/widgets/controls.hpp>
#include <windows.h>
#include <iostream>
namespace {
void need(bool yes,const char*message){if(!yes)throw std::runtime_error(message);}
struct Editor final:cell::ui::Editor {
 std::function<void()>duringTick,duringPaint;std::vector<char32_t>characters;int cursor{};
 int pointerCursor()const noexcept override{return cursor;}
 std::vector<cell::ui::PointerEvent>pointers;unsigned cancellations{};
 bool pointer(const cell::ui::PointerEvent&event)override{pointers.push_back(event);if(event.action==cell::ui::PointerAction::cancel)++cancellations;return false;}
 void paint(cell::ui::Canvas&canvas,cell::ui::Rect area)override{canvas.fill(area,cell::ui::Color(0xff294852));if(duringPaint){auto action=std::move(duringPaint);action();}}
 bool tick()override{if(duringTick){auto action=std::move(duringTick);action();return true;}return false;}
 bool keyboard(const cell::ui::KeyEvent&key)override{characters.push_back(key.text);return true;}
 cell::ui::Point constrainSize(cell::ui::Point p)const noexcept override{return p;}
 bool preserveAspectRatio()const noexcept override{return false;}
};
void checkPointerModifiers(HWND handle,Editor&editor,double scale){
 struct KeyboardRestore{
  BYTE state[256]{};
  KeyboardRestore(){need(GetKeyboardState(state)!=0,"Cannot capture the test thread's keyboard state");}
  ~KeyboardRestore(){SetKeyboardState(state);}
 }restore;
 for(unsigned mask=0;mask<8;++mask){
  BYTE state[256]{};
  state[VK_SHIFT]=mask&1?0x80:0;
  state[VK_CONTROL]=mask&2?0x80:0;
  state[VK_MENU]=mask&4?0x80:0;
  need(SetKeyboardState(state)!=0,"Cannot set the test thread's modifier state");
  for(const UINT message:{WM_MOUSEWHEEL,WM_LBUTTONDOWN,WM_MOUSEMOVE,WM_LBUTTONUP,WM_RBUTTONDOWN}){
   POINT point{120,80};
   if(message==WM_MOUSEWHEEL)ClientToScreen(handle,&point);
   const auto before=editor.pointers.size();
   SendMessageW(handle,message,message==WM_MOUSEWHEEL?MAKEWPARAM((mask&1?MK_SHIFT:0)|(mask&2?MK_CONTROL:0),WHEEL_DELTA):0,MAKELPARAM(point.x,point.y));
   need(editor.pointers.size()>before,"Native mouse message did not reach the editor");
   const auto expected=message==WM_MOUSEWHEEL?cell::ui::PointerAction::wheel:
       message==WM_LBUTTONDOWN?cell::ui::PointerAction::down:
       message==WM_LBUTTONUP?cell::ui::PointerAction::up:
       message==WM_RBUTTONDOWN?cell::ui::PointerAction::context:cell::ui::PointerAction::move;
   // Normal release must deliver up without an extra lifecycle cancellation.
   const auto found=std::find_if(editor.pointers.begin()+before,editor.pointers.end(),
                                 [expected](const auto&e){return e.action==expected;});
   need(found!=editor.pointers.end(),"Native mouse action was not delivered");
   const auto&event=*found;
   need(event.fine==bool(mask&1)&&event.control==bool(mask&2)&&event.alt==bool(mask&4),
        "Native mouse dispatch dropped Shift/Ctrl/Alt modifiers");
   need(std::abs(event.position.x-120/scale)<1.e-9&&std::abs(event.position.y-80/scale)<1.e-9,"Mouse modifier dispatch changed pointer coordinates");
   if(message==WM_MOUSEWHEEL)need(event.action==cell::ui::PointerAction::wheel&&event.wheel==1,
                                "Native wheel dispatch changed its direction or magnitude");
  }
 }
 BYTE released[256]{};need(SetKeyboardState(released)!=0,"Cannot release test modifiers");
 POINT point{120,80};ClientToScreen(handle,&point);
 SendMessageW(handle,WM_MOUSEWHEEL,MAKEWPARAM(0,WHEEL_DELTA),MAKELPARAM(point.x,point.y));
 const auto&event=editor.pointers.back();
 need(!event.fine&&!event.control&&!event.alt,"Released mouse modifiers stayed latched");
}
}
int main(){try{
 for(int mode=0;mode<2;++mode)for(int attempt=0;attempt<20;++attempt){
  Editor editor;auto window=std::make_unique<cell::ui::Window>(editor);need(window->open(0,320,200),"Cannot create native Windows fixture");window->show(true);window->poll();const auto handle=reinterpret_cast<HWND>(window->nativeHandle());
  if(mode==0){editor.duringTick=[&]{window.reset();};PostMessageW(handle,WM_TIMER,1,0);auto*borrow=window.get();borrow->poll();}
  else{editor.duringPaint=[&]{window.reset();};
   // An OS exposure reuses the cached raster. Mark the editor's pixels dirty
   // to exercise deletion during actual application painting.
   editor.invalidate();UpdateWindow(handle);}
  need(!window&&!IsWindow(handle)&&editor.nativeWindowHandle==0,"Native event-driven window deletion retained a dead HWND");
 }
 Editor editor;cell::ui::Window window(editor);need(window.open(0,320,200),"Cannot create native Unicode fixture");window.show(true);window.poll();const auto handle=reinterpret_cast<HWND>(window.nativeHandle());SendMessageW(handle,WM_CHAR,0xd83c,0);SendMessageW(handle,WM_CHAR,0xdfa4,0);need(editor.characters.size()==1&&editor.characters[0]==0x1f3a4,"Native Windows input split a supplementary Unicode character");
 checkPointerModifiers(handle,editor,window.contentScaleFactor());
 const auto beforeClick=editor.cancellations;
 SendMessageW(handle,WM_LBUTTONDOWN,MK_LBUTTON,MAKELPARAM(100,80));SendMessageW(handle,WM_LBUTTONUP,0,MAKELPARAM(100,80));
 need(editor.cancellations==beforeClick,"Ordinary click release cancelled editor keyboard focus");
 const auto other=CreateWindowExW(0,L"STATIC",L"Capture owner",WS_POPUP,0,0,10,10,nullptr,nullptr,GetModuleHandleW(nullptr),nullptr);need(other!=nullptr,"Cannot create competing capture fixture");
 SendMessageW(handle,WM_LBUTTONDOWN,MK_LBUTTON,MAKELPARAM(100,80));SetCapture(other);
 need(editor.cancellations==beforeClick+1,"Unexpected capture theft did not cancel the held editor gesture");ReleaseCapture();DestroyWindow(other);
 SendMessageW(handle,WM_LBUTTONUP,0,MAKELPARAM(100,80));need(editor.cancellations==beforeClick+1,"Release after capture theft cancelled twice");
 for(const auto [type,id]:{std::pair{8,IDC_SIZEWE},std::pair{9,IDC_SIZENS},std::pair{0,IDC_ARROW}}){editor.cursor=type;SendMessageW(handle,WM_MOUSEMOVE,0,MAKELPARAM(120,80));need(GetCursor()==LoadCursorW(nullptr,reinterpret_cast<LPCWSTR>(id)),"Native pointer dispatch ignored the editor resize cursor");SetCursor(nullptr);SendMessageW(handle,WM_SETCURSOR,reinterpret_cast<WPARAM>(handle),MAKELPARAM(HTCLIENT,WM_MOUSEMOVE));need(GetCursor()==LoadCursorW(nullptr,reinterpret_cast<LPCWSTR>(id)),"WM_SETCURSOR replaced the editor resize cursor");}
 const auto text=cell::ui::widgets::Text::fromUTF8(u8"Cell — 声 🎤 é Ж");cell::ui::widgets::Clipboard::setText(text);need(cell::ui::widgets::Clipboard::getText()==text,"Native Windows clipboard lost UTF-16/UTF-8 characters");
 need(window.setContentScaleFactor(1.25)&&window.contentScaleFactor()==1.25,"Native Windows fractional DPI failed");window.close();need(!window.isOpen(),"Native Windows explicit close retained its handle");
 cell::ui::shutdownNativePlatformServices();
 cell::ui::widgets::Clipboard::setText(text);need(cell::ui::widgets::Clipboard::getText()==text,"Native Windows clipboard failed after module service reentry");
 cell::ui::shutdownNativePlatformServices();
 std::cout<<"PASS native Windows UI: reentrant window deletion during timer and paint, userdata/handle cleanup, supplementary Unicode input, system clipboard and fractional DPI\n";return 0;
}catch(const std::exception&e){std::cerr<<"FAIL native Windows UI: "<<e.what()<<'\n';return 1;}}
