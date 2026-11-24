#pragma once

#include <cell/plugin.hpp>
#include <cell/commands.hpp>
#include <functional>
#include <string_view>

namespace cell::ui {
#if defined(_WIN32)
// The format's last module exit calls this on its GUI/lifecycle thread, after
// views close and before the OS loader lock. Lazy services can reopen on entry.
void shutdownNativePlatformServices();
#endif

struct Point { double x{}, y{}; };
struct Rect {
    double x{}, y{}, width{}, height{};
    bool contains(Point p) const noexcept { return p.x >= x && p.y >= y && p.x < x + width && p.y < y + height; }
    Rect inset(double amount) const noexcept { return {x+amount, y+amount, std::max(0.0,width-2*amount), std::max(0.0,height-2*amount)}; }
};
struct Color {
    std::uint32_t argb = 0xff000000;
    constexpr explicit Color(std::uint32_t value) : argb(value) {}
};
struct GradientStop { double offset{}; Color color{0xff000000}; };
class Surface;
struct VectorCommand {
    enum class Kind {move,line,quadratic,cubic,close};
    Kind kind{};
    Point first,second,third;
};
struct TextMetrics {double advance{},ascent{},descent{},left{},top{},right{},bottom{};};
enum class Typeface {interfaceText,brandGrotesk,brandCondensed,monospace};
struct DecodedImage {int width{},height{};std::vector<std::uint32_t> pixels;};
// Worker-safe image decoding, bounded before allocating a full decoded frame.
DecodedImage decodeImage(std::span<const std::byte>,std::size_t maximumPixels=16u*1024*1024,unsigned maximumEdge=0);

class Canvas {
public:
    explicit Canvas(void* nativeContext, void* fontFace,void* boldFace=nullptr,void* monoFace=nullptr,void* brandGrotesk=nullptr,void* brandCondensed=nullptr);
    void save();
    void restore();
    void scale(double x, double y);
    void translate(double x, double y);
    void skew(double x,double y);
    void rotate(double radians);
    void transform(double a,double b,double c,double d,double e,double f);
    void opacity(double amount);
    void clip(Rect);
    void fill(Rect, Color);
    void rounded(Rect, double radius, Color);
    void strokeRounded(Rect,double radius,Color,double width=1);
    void gradient(Rect, Color top, Color bottom, double radius = 0);
    void gradient(Rect,std::span<const GradientStop>,Point from,Point to,double radius=0,bool ellipse=false);
    void gradientPolygon(std::span<const Point>,Color first,Color last,Point from,Point to);
    void circle(Point, double radius, Color);
    void strokeCircle(Point,double radius,Color,double width=1);
    void ellipse(Rect,Color,double strokeWidth=0);
    void path(std::span<const VectorCommand>,Color,double strokeWidth=0,bool rounded=false);
    void pathGradient(std::span<const VectorCommand>,std::span<const GradientStop>,Point from,Point to,bool radial=false);
    void line(Point from, Point to, Color, double thickness = 1);
    void polyline(std::span<const Point>, Color, double thickness = 1, bool closed = false, bool filled = false);
    void text(Point baseline, std::string_view, double size, Color);
    void textWeighted(Point baseline,std::string_view,double size,Color,float weight,Typeface=Typeface::interfaceText);
    void fittedText(Rect,std::string_view,Color,float weight=400,double shear=0,Typeface=Typeface::interfaceText);
    double textWidth(std::string_view,double size,float weight=400,Typeface=Typeface::interfaceText)const;
    TextMetrics textMetrics(std::string_view,double size,float weight=400,Typeface=Typeface::interfaceText)const;
    // Font coverage alone does not qualify bidi, shaping or input support.
    std::size_t missingGlyphs(std::string_view,Typeface=Typeface::interfaceText)const;
    void textMono(Point baseline,std::string_view,double size,Color);
    double monoWidth(std::string_view,double size)const;
    bool svgPath(std::string_view,Rect,Color,double stroke=1.8);
    void image(const Surface&,Rect destination);
    void image(const Surface&,Rect destination,Rect source,bool nearest);
private:
    void* context_{};
    void* fontFace_{};
    void* boldFace_{};
    void* monoFace_{};
    void* brandGrotesk_{};void* brandCondensed_{};
    void*face(Typeface,float weight=400)const noexcept;
};

class Surface {
public:
    class PaintSession {
      public:
        ~PaintSession();
        Canvas& canvas() noexcept;
      private:
        friend class Surface;
        struct Impl;
        explicit PaintSession(Surface&);
        std::unique_ptr<Impl> impl_;
    };
    Surface();
    ~Surface();
    Surface(const Surface&) = delete;
    Surface& operator=(const Surface&) = delete;
    void resize(int width, int height);
    void render(const std::function<void(Canvas&)>&);
    std::unique_ptr<PaintSession> beginPaint();
    // GUI thread only. Packed premultiplied ARGB32, tightly packed input rows.
    void setPixels(std::span<const std::uint32_t>,int width,int height);
    bool writePng(const std::string& path) const;
    const void* pixels() const;
    std::intptr_t stride() const;
    int width() const;
    int height() const;
private:
    friend class Canvas;
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

enum class PointerAction { down, move, up, wheel, context, cancel };
enum class PointerKind { mouse, touch, pen };
enum class PointerButton {primary,secondary,middle};
struct PointerIdentity {
    std::uint64_t value{};
    PointerKind kind=PointerKind::mouse;
    bool operator==(const PointerIdentity&) const = default;
};
struct PointerEvent {
    PointerAction action{};
    Point position;
    double wheel{};
    bool fine{};
    bool reset{};
    PointerIdentity identity;
    bool control{},alt{},command{};
    PointerButton button=PointerButton::primary;
};
namespace keys {
inline constexpr int up=1000,down=1001,left=1002,right=1003,reverseTab=1004,
    contextMenu=1005,home=1006,end=1007,deleteKey=127;
}
struct KeyEvent {
    int key{};
    bool shift{},control{},alt{},command{},repeat{};
    char32_t text{};
};
enum class AccessibleAction { focus,activate,increment,decrement,setValue };
struct ControlInfo {
    enum class Role {slider,textbox,button,choice,group};
    ParamId id{};Rect bounds;Role role{};std::string label;bool enabled=true;
    // Editor identities are independent of host parameter IDs. A custom view
    // supplies stable identities for buttons and multiple faces of a parameter.
    std::uint64_t identity{};
    std::string accessibleName,valueText,unit;
    double value{},minimum{},maximum{};
    bool hasValue{};
};

// Input, control inspection and accessibility actions run on the GUI thread.
// Native bridges must copy snapshots and marshal calls from other threads.
class Editor {
public:
    // True when product callbacks address a separate audio component rather
    // than the Engine passed to createEditor (for example VST3 controllers).
    bool remoteProductState{};
    virtual ~Editor() = default;
    virtual void paint(Canvas&, Rect bounds) = 0;
    virtual bool pointer(const PointerEvent&) = 0;
    virtual bool key(int) { return false; }
    virtual bool keyboard(const KeyEvent& event) { return key(event.key); }
    // Kept separate so existing editor shortcuts do not fire again on key-up.
    virtual bool keyReleased(const KeyEvent&) { return false; }
    virtual void cancelInput() { pointer({PointerAction::cancel}); }
    virtual bool accessibleAction(std::uint64_t,AccessibleAction,double=0) { return false; }
    virtual std::uint64_t focusedControl()const noexcept{return 0;}
    virtual bool tick() { return false; }
    virtual int pointerCursor()const noexcept{return 0;}
    // Native GUI thread: a format may fetch display data before the editor ticks.
    bool advanceFrame() { if (synchronize) synchronize(); return tick(); }
    virtual void visibility(bool value) { if (visibilityChanged) visibilityChanged(value); }
    virtual const char* title() const noexcept { return "Cell / Interface Gallery"; }
    virtual Point preferredSize()const noexcept{return {1000,620};}
    virtual bool preserveAspectRatio()const noexcept{return true;}
    virtual std::span<const ControlInfo> controls()const noexcept{return {};}
    virtual Point constrainSize(Point p)const noexcept{p.x=std::clamp(p.x,500.,4000.);p.y=p.x*620/1000;return p;}
    std::uintptr_t nativeWindowHandle{};double nativeWindowScale=1;
    std::function<bool(std::span<const std::string>,bool)>externalFilesDrag;
    std::function<bool(std::span<const std::string>,Point)>onFilesDropped;
    std::function<void()> invalidate;
    std::function<void()> synchronize;
    std::function<void()> editsAvailable;
    std::function<void(bool)> visibilityChanged;
    std::function<bool(int,int)> requestSize;
    std::function<bool(std::span<const std::byte>)> restoreProductState;
    // Split-component formats acknowledge the processor's actual restore.
    // Acceptance alone must not advance an undo cursor or swap A/B snapshots.
    std::function<std::uint64_t(std::span<const std::byte>)> requestProductRestore;
    std::function<RestoreStatus(std::uint64_t)> productRestoreStatus;
    std::function<std::uint64_t(std::span<const std::byte>)> requestProductCommand;
    std::function<RestoreStatus(std::uint64_t)> productCommandStatus;
    std::function<bool(std::span<const std::byte>)> productCommand;
    // Ephemeral gestures such as controller MIDI. Adapters route these to
    // the component without marking a preset/project as edited.
    std::function<bool(std::span<const std::byte>)> transientInput;
    // GUI thread: request a read-only snapshot; an asynchronous format may
    // return no data until a later poll of the same request. The second
    // argument discards prior replies when an action requires a fresh result.
    std::function<std::vector<std::byte>(std::span<const std::byte>,bool)> productQuery;
    // GUI thread. Probe at use time: host support can change on attachment.
    // Positions use the same surface coordinates as PointerEvent; adapters
    // translate them to the native view before opening the host's own menu.
    std::function<bool(ParamId)> hostParameterMenuAvailable;
    std::function<bool(ParamId,Point)> showHostParameterMenu;
};

// Native window and embedded editor share the same Surface and input path.
// All methods must run on the host's GUI thread. No private GUI/audio threads.
class Window {
public:
    explicit Window(Editor&);
    ~Window();
    bool open(std::uintptr_t parent = 0, int width = 1000, int height = 620);
    void close();
    void setPosition(int x,int y);
    void setSizeLimits(int minWidth,int minHeight,int maxWidth,int maxHeight);
    void show(bool visible);
    bool resize(int width, int height);
    bool beginFileDrag(std::span<const std::string>,bool move=false);
    // Open/resize use native parent-window coordinates. Editor painting,
    // hit testing and control bounds use logical coordinates. Cocoa raster
    // backing density is separate from this host-provided content scale.
    bool setContentScaleFactor(double);
    double contentScaleFactor()const noexcept;
    void poll();
    bool isOpen() const;
    std::uintptr_t nativeHandle() const;
    Point toNativePoint(Point surfacePoint)const;
private:
    struct Impl;
    std::shared_ptr<Impl> impl_;
};

} // namespace cell::ui

namespace cell::plugin {
// Each lab profile supplies its own editor behind the same format adapter.
std::unique_ptr<ui::Editor> createEditor(Engine&);
}
