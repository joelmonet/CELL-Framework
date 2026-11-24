// Original Cell Framework implementation; see LICENSE.
#include <cell/widgets/services.hpp>
#include <X11/Xlib.h>
#include <X11/keysym.h>
#undef KeyPress
#include <iostream>
#include <chrono>
#include <thread>
using namespace cell::ui;namespace w=cell::ui::widgets;
namespace {void need(bool v,const char*m){if(!v)throw std::runtime_error(m);}
struct Host:w::WidgetHost {using WidgetHost::WidgetHost;int functionKey{};bool keyboard(const KeyEvent&e)override{if(e.key==1111)++functionKey;return WidgetHost::keyboard(e);}};
void key(Display*d,std::uintptr_t target,int type,KeySym symbol,unsigned state){XEvent event{};event.xkey.type=type;event.xkey.display=d;event.xkey.window=target;event.xkey.root=DefaultRootWindow(d);event.xkey.keycode=XKeysymToKeycode(d,symbol);event.xkey.state=state;event.xkey.same_screen=True;XSendEvent(d,target,False,0,&event);XSync(d,False);}
void click(Display*d,std::uintptr_t target,int type,int x,int y){XEvent event{};event.xbutton.type=type;event.xbutton.display=d;event.xbutton.window=target;event.xbutton.root=DefaultRootWindow(d);event.xbutton.button=Button1;event.xbutton.x=x;event.xbutton.y=y;event.xbutton.same_screen=True;XSendEvent(d,target,False,0,&event);XSync(d,False);}
void awaitActive(Display*d,cell::ui::Window&window){
 const auto deadline=std::chrono::steady_clock::now()+std::chrono::seconds(2);
 while(std::chrono::steady_clock::now()<deadline){
  window.poll();XWindowAttributes attributes{};::Window focus{};int revert{};
  XGetInputFocus(d,&focus,&revert);
  if(XGetWindowAttributes(d,window.nativeHandle(),&attributes)&&attributes.map_state==IsViewable){
   if(focus==window.nativeHandle()){
    // Drain the map/configure/focus events before asserting click completion.
    // FocusOut legitimately cancels an in-flight pointer gesture.
    window.poll();return;
   }
   // A window manager may decline to focus a remapped window automatically.
   // Activate it through the same native click path as the user.
   click(d,window.nativeHandle(),ButtonPress,400,250);window.poll();
   click(d,window.nativeHandle(),ButtonRelease,400,250);window.poll();
  }
  std::this_thread::sleep_for(std::chrono::milliseconds(1));
 }
 XWindowAttributes attributes{};::Window focus{},root{},parent{},*children{};unsigned count{};int revert{};
 XGetWindowAttributes(d,window.nativeHandle(),&attributes);XGetInputFocus(d,&focus,&revert);
 XQueryTree(d,window.nativeHandle(),&root,&parent,&children,&count);if(children)XFree(children);
 std::cerr<<"Native window state: mapped="<<attributes.map_state<<" focus="<<focus<<" target="<<window.nativeHandle()<<" parent="<<parent<<" root="<<root<<'\n';
 throw std::runtime_error("Window manager did not map and focus the native test window");
}
}
int main(){Display*display=nullptr;try{display=XOpenDisplay(nullptr);need(display,"Native window tests require a display");
 auto root=std::make_unique<w::Component>();root->setSize(640,400);auto button=std::make_unique<w::TextButton>("Close during input");button->setBounds(20,20,200,40);root->addAndMakeVisible(*button);auto host=std::make_unique<Host>(*root);auto window=std::make_unique<cell::ui::Window>(*host);need(window->open(0,640,400),"Native window did not open");window->setPosition(30,40);window->setSizeLimits(400,240,1600,1000);window->show(true);window->poll();const auto handle=window->nativeHandle();
 // Reopen before the manager necessarily processes the first MapRequest.
 window->show(false);window->show(true);awaitActive(display,*window);
 key(display,handle,2,XK_F11,0);window->poll();need(host->functionKey==1,"F11 did not map to the editor focus shortcut");key(display,handle,3,XK_F11,0);window->poll();key(display,handle,2,XK_Control_L,0);window->poll();key(display,handle,3,XK_Control_L,ControlMask);window->poll();need(!w::ModifierKeys::getCurrentModifiers().isCtrlDown(),"OS key release left Control down");
 for(const auto size:{std::pair{640,400},std::pair{800,500},std::pair{900,600}}){need(window->resize(size.first,size.second),"Native resize rejected");window->poll();awaitActive(display,*window);if(root->getWidth()!=size.first||root->getHeight()!=size.second){XWindowAttributes a{};XGetWindowAttributes(display,handle,&a);std::cerr<<"Resize diagnostic: requested="<<size.first<<"x"<<size.second<<" content="<<root->getWidth()<<"x"<<root->getHeight()<<" server="<<a.width<<"x"<<a.height<<" mapped="<<a.map_state<<"\n";}need(root->getWidth()==size.first&&root->getHeight()==size.second,"A stale native configure replaced the requested size");}
 // Synthetic replies from a different client have a current serial even
 // when they describe an old configure request. They must not undo the latest
 // local resize before the manager has acknowledged it.
 need(window->resize(1000,650),"Latest native resize rejected");
 XEvent stale{};stale.xconfigure.type=ConfigureNotify;stale.xconfigure.display=display;
 stale.xconfigure.event=handle;stale.xconfigure.window=handle;stale.xconfigure.width=640;stale.xconfigure.height=400;
 XSendEvent(display,handle,False,StructureNotifyMask,&stale);XSync(display,False);
 window->poll();awaitActive(display,*window);
 need(root->getWidth()==1000&&root->getHeight()==650,"A current-serial stale manager reply changed the latest requested size");
 // A manager can legitimately constrain a request. Retaining the latest size
 // must not leave its logical surface permanently larger than the real peer.
 window->setSizeLimits(400,240,1000,650);need(window->resize(1200,800),"Constrained native resize rejected locally");
 const auto settle=std::chrono::steady_clock::now()+std::chrono::milliseconds(350);
 while(std::chrono::steady_clock::now()<settle){window->poll();std::this_thread::sleep_for(std::chrono::milliseconds(1));}
 XWindowAttributes constrained{};need(XGetWindowAttributes(display,handle,&constrained),"Constrained peer disappeared");
 need(constrained.width<=1000&&constrained.height<=650&&root->getWidth()==constrained.width&&root->getHeight()==constrained.height,"Denied resize did not reconcile to the manager's actual constraints");
 // A pointer event queued before hiding must not ask X11 to focus an unmapped
 // window (BadMatch terminates clients using Xlib's default error handler).
 window->show(false);window->poll();click(display,handle,ButtonPress,400,250);window->poll();click(display,handle,ButtonRelease,400,250);window->poll();XSync(display,False);window->show(true);awaitActive(display,*window);
 int closed=0;button->onClick=[&]{++closed;window.reset();host.reset();root.reset();};click(display,handle,ButtonPress,50,35);window->poll();click(display,handle,ButtonRelease,50,35);window->poll();need(closed==1&&!window&&!host&&!root,"Native input could not close its own window safely");
 w::DocumentWindow document("Cell native resize/reopen",w::Colour{0xff141719},w::DocumentWindow::closeButton);auto*body=new w::Component;body->setSize(680,420);document.setContentOwned(body,true);document.centreWithSize(680,420);document.setVisible(true);w::Timer::dispatchPending();need(document.nativeHandle()!=0,"Native document peer missing");document.setVisible(false);document.setVisible(true);document.setSize(740,460);w::Timer::dispatchPending();need(body->getWidth()==740&&body->getHeight()==460,"Native document resize did not reach content");document.setVisible(false);XCloseDisplay(display);display=nullptr;
 std::cout<<"PASS native window: OS F11 mapping, released modifiers, unmapped focus guard, input callback destroys its own window, reopen and content resize\n";return 0;
 }catch(const std::exception&e){if(display)XCloseDisplay(display);std::cerr<<e.what()<<'\n';return 1;}}
