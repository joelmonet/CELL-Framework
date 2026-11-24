#include <cell/ui.hpp>
#include <cell/ui_scaling.hpp>
#import <Cocoa/Cocoa.h>
#import <CoreGraphics/CoreGraphics.h>

namespace {
struct MacState {
    cell::ui::Editor& editor;cell::ui::Surface surface;
    bool open{},standalone{},dirty=true;double contentScale=1;
    explicit MacState(cell::ui::Editor& e):editor(e){}
};
}
@interface CellNativeEditorView01 : NSView<NSWindowDelegate>
@property(nonatomic,assign) void* state;
@property(nonatomic,strong) NSTimer* cellTimer;
@end
@implementation CellNativeEditorView01
- (BOOL)isFlipped{return YES;}
- (BOOL)acceptsFirstResponder{return YES;}
- (void)drawRect:(NSRect)rect {
    (void)rect;auto* s=static_cast<MacState*>(self.state);if(!s)return;
    const CGFloat scale=self.window.backingScaleFactor>0?self.window.backingScaleFactor:1;
    const int width=int(self.bounds.size.width*scale),height=int(self.bounds.size.height*scale);
    if(width<1||height<1||width>8192||height>8192)return;
    if(s->surface.width()!=width||s->surface.height()!=height){s->editor.cancelInput();s->surface.resize(width,height);s->dirty=true;}
    if(s->dirty||s->editor.advanceFrame()){s->surface.render([&](cell::ui::Canvas& c){c.scale(scale*s->contentScale,scale*s->contentScale);s->editor.paint(c,{0,0,self.bounds.size.width/s->contentScale,self.bounds.size.height/s->contentScale});});s->dirty=false;}
    CGColorSpaceRef space=CGColorSpaceCreateDeviceRGB();
    CGDataProviderRef provider=CGDataProviderCreateWithData(nullptr,s->surface.pixels(),size_t(s->surface.stride())*height,nullptr);
    CGImageRef image=CGImageCreate(width,height,8,32,size_t(s->surface.stride()),space,
        CGBitmapInfo(kCGBitmapByteOrder32Little|kCGImageAlphaPremultipliedFirst),provider,nullptr,false,kCGRenderingIntentDefault);
    CGContextRef context=NSGraphicsContext.currentContext.CGContext;
    CGContextSaveGState(context);CGContextTranslateCTM(context,0,self.bounds.size.height);CGContextScaleCTM(context,1,-1);
    CGContextDrawImage(context,CGRectMake(0,0,self.bounds.size.width,self.bounds.size.height),image);CGContextRestoreGState(context);
    CGImageRelease(image);CGDataProviderRelease(provider);CGColorSpaceRelease(space);
}
- (void)cellTick:(NSTimer*)timer {
    (void)timer;auto* s=static_cast<MacState*>(self.state);
    if(s&&!self.hidden&&self.window.visible&&(s->dirty||s->editor.advanceFrame()))[self setNeedsDisplay:YES];
}
- (void)cellPointer:(NSEvent*)event action:(cell::ui::PointerAction)action {
    auto* s=static_cast<MacState*>(self.state);if(!s)return;
    const NSPoint point=[self convertPoint:event.locationInWindow fromView:nil];
    const double wheel=action==cell::ui::PointerAction::wheel?event.scrollingDeltaY:0;
    s->editor.pointer({action,{point.x/s->contentScale,point.y/s->contentScale},wheel,
        bool(event.modifierFlags&NSEventModifierFlagShift),event.clickCount==2,{},bool(event.modifierFlags&NSEventModifierFlagControl),bool(event.modifierFlags&NSEventModifierFlagOption),bool(event.modifierFlags&NSEventModifierFlagCommand)});
}
- (void)mouseDown:(NSEvent*)event{[self.window makeFirstResponder:self];[self cellPointer:event action:cell::ui::PointerAction::down];}
- (void)rightMouseDown:(NSEvent*)event{[self cellPointer:event action:cell::ui::PointerAction::context];}
- (void)mouseDragged:(NSEvent*)event{[self cellPointer:event action:cell::ui::PointerAction::move];}
- (void)mouseUp:(NSEvent*)event{[self cellPointer:event action:cell::ui::PointerAction::up];}
- (void)scrollWheel:(NSEvent*)event{[self cellPointer:event action:cell::ui::PointerAction::wheel];}
- (BOOL)resignFirstResponder{auto* s=static_cast<MacState*>(self.state);if(s)s->editor.cancelInput();return [super resignFirstResponder];}
- (void)keyDown:(NSEvent*)event{auto* s=static_cast<MacState*>(self.state);if(!s)return;const bool shift=bool(event.modifierFlags&NSEventModifierFlagShift);int key=event.keyCode==48?9:event.keyCode==126?cell::ui::keys::up:event.keyCode==125?cell::ui::keys::down:event.keyCode==123?cell::ui::keys::left:event.keyCode==124?cell::ui::keys::right:event.keyCode==117?cell::ui::keys::deleteKey:event.keyCode==115?cell::ui::keys::home:event.keyCode==119?cell::ui::keys::end:event.keyCode==109&&shift?cell::ui::keys::contextMenu:0;
    NSString* characters=event.charactersIgnoringModifiers;if(!key&&characters.length){key=[characters characterAtIndex:0];if(key==127)key=8;}
    if(!s->editor.keyboard({key,shift,bool(event.modifierFlags&NSEventModifierFlagControl),bool(event.modifierFlags&NSEventModifierFlagOption),bool(event.modifierFlags&NSEventModifierFlagCommand),bool(event.isARepeat)}))[super keyDown:event];}
- (void)keyUp:(NSEvent*)event{auto* s=static_cast<MacState*>(self.state);if(!s)return;NSString* characters=event.charactersIgnoringModifiers;bool handled=false;if(characters.length)handled=s->editor.keyReleased({int([characters characterAtIndex:0]),bool(event.modifierFlags&NSEventModifierFlagShift),bool(event.modifierFlags&NSEventModifierFlagControl),bool(event.modifierFlags&NSEventModifierFlagOption),bool(event.modifierFlags&NSEventModifierFlagCommand),false});if(!handled)[super keyUp:event];}

- (void)windowWillClose:(NSNotification*)notification{(void)notification;auto* s=static_cast<MacState*>(self.state);if(s){s->editor.visibility(false);s->open=false;}}
@end

namespace cell::ui {
struct Window::Impl : MacState {
    __strong CellNativeEditorView01* view=nil;
    __strong NSWindow* window=nil;
    explicit Impl(Editor& e):MacState(e){editor.invalidate=[this]{dirty=true;if(view)[view setNeedsDisplay:YES];};}
    ~Impl(){editor.invalidate={};close();}
    void close(){editor.nativeWindowHandle=0;editor.visibility(false);if(view){[view.cellTimer invalidate];view.cellTimer=nil;view.state=nullptr;[view removeFromSuperview];view=nil;}
        if(window){window.delegate=nil;[window close];window=nil;}open=false;}
};
Window::Window(Editor& e):impl_(std::make_unique<Impl>(e)){}
Window::~Window()=default;
bool Window::open(std::uintptr_t parent,int width,int height){
    auto& p=*impl_;if(p.open||width<1||height<1||width>4096||height>4096)return false;
    p.standalone=parent==0;
    if(p.standalone){[NSApplication sharedApplication];[NSApp setActivationPolicy:NSApplicationActivationPolicyRegular];[NSApp finishLaunching];}
    p.view=[[CellNativeEditorView01 alloc]initWithFrame:NSMakeRect(0,0,width,height)];p.view.state=&p;
    if(parent){NSView* parentView=(__bridge NSView*)reinterpret_cast<void*>(parent);[parentView addSubview:p.view];}
    else{p.window=[[NSWindow alloc]initWithContentRect:NSMakeRect(0,0,width,height)
        styleMask:NSWindowStyleMaskTitled|NSWindowStyleMaskClosable|NSWindowStyleMaskResizable
        backing:NSBackingStoreBuffered defer:NO];p.window.releasedWhenClosed=NO;p.window.title=[NSString stringWithUTF8String:p.editor.title()];p.window.contentView=p.view;p.window.delegate=p.view;[p.window center];}
    p.editor.nativeWindowHandle=reinterpret_cast<std::uintptr_t>((__bridge void*)p.view);p.open=true;p.dirty=true;return true;
}
void Window::close(){impl_->close();}
void Window::setPosition(int x,int y){auto&p=*impl_;if(p.window){const auto frame=p.window.frame;const auto screen=p.window.screen?:NSScreen.mainScreen;[p.window setFrameOrigin:NSMakePoint(x,NSMaxY(screen.frame)-y-frame.size.height)];}else if(p.view)[p.view setFrameOrigin:NSMakePoint(x,y)];}
void Window::setSizeLimits(int a,int b,int c,int d){auto&p=*impl_;if(p.window){p.window.contentMinSize=NSMakeSize(std::max(1,a),std::max(1,b));p.window.contentMaxSize=NSMakeSize(std::max(a,c),std::max(b,d));}}
void Window::show(bool visible){auto& p=*impl_;if(!p.open)return;p.view.hidden=!visible;
    p.editor.visibility(visible);
    if(p.window){if(visible){[p.window makeKeyAndOrderFront:nil];[NSApp activateIgnoringOtherApps:YES];}else[p.window orderOut:nil];}
    [p.view.cellTimer invalidate];p.view.cellTimer=nil;
    if(visible){p.view.cellTimer=[NSTimer timerWithTimeInterval:1./30 target:p.view selector:@selector(cellTick:) userInfo:nil repeats:YES];
        [[NSRunLoop mainRunLoop]addTimer:p.view.cellTimer forMode:NSRunLoopCommonModes];[p.view setNeedsDisplay:YES];}}
bool Window::resize(int width,int height){auto& p=*impl_;if(!p.open||width<1||height<1||width>4096||height>4096)return false;
    if(p.window)[p.window setContentSize:NSMakeSize(width,height)];else[p.view setFrameSize:NSMakeSize(width,height)];p.dirty=true;return true;}
void Window::poll(){auto& p=*impl_;if(!p.open)return;
    if(p.standalone){NSEvent* event;while((event=[NSApp nextEventMatchingMask:NSEventMaskAny untilDate:[NSDate distantPast] inMode:NSDefaultRunLoopMode dequeue:YES]))[NSApp sendEvent:event];[NSApp updateWindows];}
    if(p.view&&(p.dirty||p.editor.advanceFrame()))[p.view setNeedsDisplay:YES];}
bool Window::isOpen()const{return impl_->open;}
std::uintptr_t Window::nativeHandle()const{return reinterpret_cast<std::uintptr_t>((__bridge void*)impl_->view);}
bool Window::setContentScaleFactor(double factor){
    auto& p=*impl_;if(!validContentScale(factor))return false;if(factor==p.contentScale)return true;
    p.editor.cancelInput();p.contentScale=factor;p.dirty=true;if(p.view)[p.view setNeedsDisplay:YES];return true;
}
double Window::contentScaleFactor()const noexcept{return impl_->contentScale;}
Point Window::toNativePoint(Point point)const{return nativePoint(point,impl_->contentScale);}
}
