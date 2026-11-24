// Original Cell Framework implementation; see LICENSE.
#include <cell/widgets/services.hpp>
#include <X11/Xlib.h>
#include <X11/Xutil.h>
#include <chrono>
#include <iostream>
namespace w=cell::ui::widgets;
namespace {void need(bool value,const char* message){if(!value)throw std::runtime_error(message);}
struct Body:w::Component{void paint(w::Graphics& graphics)override{graphics.fillAll(w::Colour{0xff2a78cf});}};
}
int main(){Display*display=nullptr;try{
    display=XOpenDisplay(nullptr);need(display,"Visual qualification needs an X display");
    XVisualInfo visual{};need(XMatchVisualInfo(display,DefaultScreen(display),32,TrueColor,&visual),"Visual qualification requires an ARGB32 host visual");
    XSetWindowAttributes attributes{};attributes.colormap=XCreateColormap(display,DefaultRootWindow(display),visual.visual,AllocNone);attributes.border_pixel=0;attributes.background_pixel=0;attributes.override_redirect=True;
    const auto parent=XCreateWindow(display,DefaultRootWindow(display),0,0,1000,700,0,visual.depth,InputOutput,visual.visual,CWColormap|CWBorderPixel|CWBackPixel|CWOverrideRedirect,&attributes);
    XMapWindow(display,parent);XSync(display,False);
    {
        Body body;body.setSize(640,400);w::WidgetHost host(body);cell::ui::Window child(host);
        need(child.open(parent,640,400),"Cell child could not inherit the host visual");child.show(true);
        for(const auto size:{std::pair{640,400},std::pair{800,500}}){
            need(child.resize(size.first,size.second),"Embedded visual resize failed");
            const auto deadline=std::chrono::steady_clock::now()+std::chrono::seconds(2);bool painted=false;
            do{
                child.poll();XSync(display,False);
                XWindowAttributes current{};need(XGetWindowAttributes(display,child.nativeHandle(),&current),"Embedded child attributes are missing");
                if(current.map_state!=IsViewable)continue;
                auto* image=XGetImage(display,child.nativeHandle(),20,20,1,1,AllPlanes,ZPixmap);
                need(image,"Embedded host child could not be sampled");
                const auto pixel=XGetPixel(image,0,0);XDestroyImage(image);
                painted=(pixel&0xffffffUL)==0x2a78cfUL;
            }while(!painted&&std::chrono::steady_clock::now()<deadline);
            need(painted,"Embedded ARGB32 child changed RGB values or failed to paint");
        }
        child.close();
    }
    XDestroyWindow(display,parent);XFreeColormap(display,attributes.colormap);XCloseDisplay(display);display=nullptr;
    std::cout<<"PASS native X11 visual: ARGB32 host embedding, matching image depth, exact RGB pixels and resize\n";return 0;
}catch(const std::exception& error){if(display)XCloseDisplay(display);std::cerr<<error.what()<<'\n';return 1;}}
