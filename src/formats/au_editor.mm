// Original Cell Framework implementation; see LICENSE.
#import <Cocoa/Cocoa.h>
#import <AudioToolbox/AUCocoaUIView.h>

#include "au_editor.hpp"
#include "cell_au_web_bindings.hpp"
#include "cell_au_web_page.hpp"
#include "cocoa_web_editor.hpp"
#include <cell/webview.hpp>
#include <map>
#include <mutex>

namespace {
using cell::au::EditorConnection;

std::mutex editorConnectionsMutex;
std::map<AudioUnit, std::weak_ptr<EditorConnection>> editorConnections;

std::shared_ptr<EditorConnection> findConnection(AudioUnit unit) {
    const std::lock_guard lock(editorConnectionsMutex);
    const auto found = editorConnections.find(unit);
    return found != editorConnections.end() ? found->second.lock() : nullptr;
}

cell::web::Bounds webBounds(NSSize size) {
    const auto extent = [](CGFloat value) {
        return std::isfinite(value) ? static_cast<int>(std::clamp(value, 1., 8192.)) : 1;
    };
    return {0, 0, extent(size.width), extent(size.height)};
}

class Editor {
public:
    explicit Editor(std::shared_ptr<EditorConnection> connection)
        : connection_(std::move(connection)), bridge_({cell::plugin::descriptor().parameters,
            [this](cell::ParamId id) { return read(id); },
            [this](cell::ParamId id) { return gesture(id, kAudioUnitEvent_BeginParameterChangeGesture); },
            [this](cell::ParamId id, double value, bool active) { return write(id, value, active); },
            [this](cell::ParamId id) { gesture(id, kAudioUnitEvent_EndParameterChangeGesture); return true; }},
            cell::designer::parameters
#if defined(CELL_DESIGNER_HAS_RULE_SOURCES)
            , {}, cell::designer::readOnlyParameters
#endif
        ) {}

    ~Editor() { setVisible(false); }

    bool open(NSView* parent) {
        cell::web::Options options;
        options.parent = reinterpret_cast<std::uintptr_t>((__bridge void*)parent);
        options.bounds = webBounds(parent.bounds.size);
        options.boundsUnits = cell::web::BoundsUnits::parentCoordinates;
        const std::string_view html(reinterpret_cast<const char*>(cell::assets::auEditorPage),
                                    sizeof(cell::assets::auEditorPage));
        return browser_.open(options) && browser_.loadHtml(html);
    }

    void setVisible(bool visible) {
        if (visible_ == visible) return;
        visible_ = visible;
#if defined(CELL_AU_EDITOR_METERS)
        if (visible) connection_->meterReaders.fetch_add(1, std::memory_order_relaxed);
        else connection_->meterReaders.fetch_sub(1, std::memory_order_relaxed);
#endif
        browser_.setVisible(visible);
        if (!visible && !polling_) bridge_.cancel();
    }

    void resize(NSSize size) { browser_.setBounds(webBounds(size)); }

    bool poll() {
        if (polling_) return true;
        polling_ = true;
        bool success{};
        try { success = refresh(); }
        catch (...) {}
        polling_ = false;
        if (!visible_) bridge_.cancel();
        return success;
    }

private:
    // The host owns the AU instance while making AU calls. Invalidate any
    // remaining views at disposal and recheck after host notifications.
    AudioUnit unit() const noexcept { return connection_->unit.load(std::memory_order_acquire); }

    double read(cell::ParamId id) {
        AudioUnitParameterValue value{};
        if (const auto audio = unit()) AudioUnitGetParameter(audio, id, kAudioUnitScope_Global, 0, &value);
        return value;
    }

    bool gesture(cell::ParamId id, AudioUnitEventType kind) {
        const auto audio = unit();
        if (!audio) return false;
        AudioUnitEvent event{kind, {{audio, id, kAudioUnitScope_Global, 0}}};
        AUEventListenerNotify(nullptr, nullptr, &event);
        return true;
    }

    bool write(cell::ParamId id, double value, bool active) {
        if (!active) gesture(id, kAudioUnitEvent_BeginParameterChangeGesture);
        const AudioUnitParameter parameter{unit(), id, kAudioUnitScope_Global, 0};
        const bool sent = visible_ && parameter.mAudioUnit &&
            AUParameterSet(nullptr, nullptr, &parameter, static_cast<AudioUnitParameterValue>(value), 0) == noErr;
        if (!active) gesture(id, kAudioUnitEvent_EndParameterChangeGesture);
        return sent;
    }

    bool refresh() {
        if (!unit()) return false;
        for (const auto& event : browser_.poll()) {
            if (!visible_ || !unit()) break;
            if (event.kind == cell::web::Event::Kind::loaded) {
                loaded_ = true;
                lastParameters_.clear();
#if defined(CELL_AU_EDITOR_METERS)
                lastMeters_.clear();
#endif
            } else if (event.kind == cell::web::Event::Kind::message && loaded_) {
                bridge_.apply(event.text);
                lastParameters_.clear();
            } else if (event.kind == cell::web::Event::Kind::error) {
                bridge_.cancel();
                if (!loaded_) return false;
            }
        }
        if (visible_ && unit() && loaded_) {
            send(bridge_.snapshot(), lastParameters_);
#if defined(CELL_AU_EDITOR_METERS)
            send(connection_->meter.snapshot(), lastMeters_);
#endif
        }
        return unit() != nullptr;
    }

    void send(std::string snapshot, std::string& last) {
        if (snapshot != last && browser_.send(snapshot)) last = std::move(snapshot);
    }

    std::shared_ptr<EditorConnection> connection_;
    cell::web::View browser_;
    cell::web::ParameterBridge bridge_;
    bool visible_{}, loaded_{}, polling_{};
    std::string lastParameters_;
#if defined(CELL_AU_EDITOR_METERS)
    std::string lastMeters_;
#endif
};
}

@interface CellAUView : NSView
- (instancetype)initWithSize:(NSSize)size connection:(std::shared_ptr<EditorConnection>)connection;
@end

@implementation CellAUView {
    NSView* _content;
    std::unique_ptr<Editor> _editor;
    NSTimer* _timer;
    __weak NSWindow* _observedWindow;
}

- (instancetype)initWithSize:(NSSize)size connection:(std::shared_ptr<EditorConnection>)connection {
    self = [super initWithFrame:NSMakeRect(0, 0, size.width, size.height)];
    if (!self) return nil;
    // The WebView retains its immediate parent. Keep it inside this wrapper,
    // whose lifetime is controlled by the Audio Unit host.
    _content = [[NSView alloc] initWithFrame:self.bounds];
    [self addSubview:_content];
    _editor = std::make_unique<Editor>(std::move(connection));
    if (!_editor->open(_content)) return nil;
    return self;
}

- (void)setFrameSize:(NSSize)size {
    [super setFrameSize:size];
    _content.frame = self.bounds;
    if (_editor) _editor->resize(self.bounds.size);
}

- (void)stopRefreshing {
    [_timer invalidate];
    _timer = nil;
    if (_editor) _editor->setVisible(false);
}

- (void)updateVisibility {
    if (!self.window || self.isHiddenOrHasHiddenAncestor) {
        [self stopRefreshing];
        return;
    }
    if (_timer || !_editor) return;
    _editor->setVisible(true);
    __weak CellAUView* weakView = self;
    _timer = [NSTimer timerWithTimeInterval:1.0 / 30.0 repeats:YES block:^(NSTimer*) {
        CellAUView* view = weakView;
        if (view && !view->_editor->poll()) [view stopRefreshing];
    }];
    [NSRunLoop.mainRunLoop addTimer:_timer forMode:NSRunLoopCommonModes];
}

- (void)windowWillClose:(NSNotification*)notification { [self stopRefreshing]; }
- (void)windowDidBecomeKey:(NSNotification*)notification { [self updateVisibility]; }

- (void)viewDidMoveToWindow {
    [super viewDidMoveToWindow];
    auto* center = NSNotificationCenter.defaultCenter;
    [center removeObserver:self name:NSWindowWillCloseNotification object:_observedWindow];
    [center removeObserver:self name:NSWindowDidBecomeKeyNotification object:_observedWindow];
    _observedWindow = self.window;
    if (_observedWindow) {
        [center addObserver:self selector:@selector(windowWillClose:) name:NSWindowWillCloseNotification object:_observedWindow];
        [center addObserver:self selector:@selector(windowDidBecomeKey:) name:NSWindowDidBecomeKeyNotification object:_observedWindow];
    }
    [self updateVisibility];
}
- (void)viewDidHide { [super viewDidHide]; [self updateVisibility]; }
- (void)viewDidUnhide { [super viewDidUnhide]; [self updateVisibility]; }
- (void)dealloc {
    [NSNotificationCenter.defaultCenter removeObserver:self];
    [_timer invalidate];
}
@end

@interface CellAUViewFactory : NSObject<AUCocoaUIBase>
@end

@implementation CellAUViewFactory
- (unsigned)interfaceVersion { return 0; }
- (NSString*)description { return [[[NSBundle bundleForClass:self.class] objectForInfoDictionaryKey:@"CFBundleName"] copy]; }
- (NSView*)uiViewForAudioUnit:(AudioUnit)unit withSize:(NSSize)preferred {
    if (!NSThread.isMainThread) return nil;
    try {
        auto connection = findConnection(unit);
        if (!connection || !cell::web::cocoa::retainPluginModule()) return nil;
        const auto& descriptor = cell::plugin::descriptor();
        if (!std::isfinite(preferred.width) || preferred.width <= 0) preferred.width = descriptor.editorWidth;
        if (!std::isfinite(preferred.height) || preferred.height <= 0) preferred.height = descriptor.editorHeight;
        const auto size = webBounds(preferred);
        return [[CellAUView alloc] initWithSize:NSMakeSize(size.width, size.height) connection:std::move(connection)];
    } catch (...) { return nil; }
}
@end

namespace cell::au {
std::shared_ptr<EditorConnection> registerEditorConnection(AudioUnit unit) {
    auto connection = std::make_shared<EditorConnection>(unit);
    const std::lock_guard lock(editorConnectionsMutex);
    editorConnections.insert_or_assign(unit, connection);
    return connection;
}

void unregisterEditorConnection(AudioUnit unit) {
    const std::lock_guard lock(editorConnectionsMutex);
    const auto found = editorConnections.find(unit);
    if (found == editorConnections.end()) return;
    if (const auto connection = found->second.lock()) connection->unit.store(nullptr, std::memory_order_release);
    editorConnections.erase(found);
}

OSStatus copyCocoaViewInfo(AudioUnitCocoaViewInfo& info) {
    const auto factory = CellAUViewFactory.class;
    NSURL* location = [NSBundle bundleForClass:factory].bundleURL;
    NSString* name = NSStringFromClass(factory);
    if (!location || !name) return kAudioUnitErr_InvalidPropertyValue;
    info = {(__bridge_retained CFURLRef)location, {(__bridge_retained CFStringRef)name}};
    return noErr;
}
}
