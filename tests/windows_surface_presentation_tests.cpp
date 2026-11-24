#include <cell/ui.hpp>
#include <windows.h>
#include <iostream>
#include <stdexcept>

namespace {
void check(bool value,const char* message){if(!value)throw std::runtime_error(message);}
struct Pattern final:cell::ui::Editor {
    bool pointer(const cell::ui::PointerEvent&)override{return false;}
    void paint(cell::ui::Canvas& c,cell::ui::Rect bounds)override{
        c.fill(bounds,cell::ui::Color(0xff102030));
        for(int y=0;y<int(std::ceil(bounds.height));++y)c.fill({0,double(y),bounds.width,1},cell::ui::Color(0xff000000u|((unsigned(y)*17u%256u)<<16)|((unsigned(y)*29u%256u)<<8)|(unsigned(y)*47u%256u)));
        for(int x=0;x<int(bounds.width);x+=37)c.fill({double(x),0,3,bounds.height},cell::ui::Color(0xffe3ac51));
    }
    bool preserveAspectRatio()const noexcept override{return false;}
};
}
int main(){try{
    const auto parent=CreateWindowExW(0,L"STATIC",L"Cell pixel presentation test",WS_POPUP|WS_VISIBLE,0,0,900,500,nullptr,nullptr,GetModuleHandleW(nullptr),nullptr);
    check(parent,"Could not create pixel-test host");
    Pattern editor;cell::ui::Window window(editor);check(window.open(std::uintptr_t(parent),320,271),"Could not open native pixel surface");window.show(true);
    for(double scale:{1.,1.25,1.5,1.75,2.})for(int width:{320,321,1001,1320,1475,1481,1499,1600,333,1320}){
        check(window.setContentScaleFactor(scale)&&window.resize(width,271),"Pixel-test resize/scale rejected");window.poll();
        const auto handle=reinterpret_cast<HWND>(window.nativeHandle());editor.invalidate();UpdateWindow(handle);
        cell::ui::Surface expected;expected.resize(width,271);expected.render([&](cell::ui::Canvas& c){c.scale(scale,scale);editor.paint(c,{0,0,width/scale,271/scale});});
        HDC dc=GetDC(handle);check(dc,"Pixel-test window DC unavailable");bool correct=true;
        for(int y:{3,17,50,119,173,223})for(int x:{1,7,31,63,127,255}){
            const auto value=*reinterpret_cast<const std::uint32_t*>(static_cast<const std::byte*>(expected.pixels())+y*expected.stride()+x*4);
            const auto actual=GetPixel(dc,x,y),wanted=RGB((value>>16)&255,(value>>8)&255,value&255);
            if(actual!=wanted){std::cerr<<"Presentation mismatch width="<<width<<" stride="<<expected.stride()<<" scale="<<scale<<" x="<<x<<" y="<<y<<" actual="<<actual<<" expected="<<wanted<<'\n';correct=false;break;}
        }
        ReleaseDC(handle,dc);check(correct,"Windows presentation ignored pixel-row padding");
    }
    window.close();DestroyWindow(parent);cell::ui::shutdownNativePlatformServices();
    std::cout<<"PASS: native Windows GDI pixels match the raster at aligned/unaligned widths, repeated resize and five fractional content scales\n";
}catch(const std::exception& e){std::cerr<<e.what()<<'\n';return 1;}}
