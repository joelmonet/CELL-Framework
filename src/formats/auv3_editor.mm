// Original Cell Framework implementation; see LICENSE.
#import <CoreAudioKit/AUViewController.h>
#import <WebKit/WebKit.h>

#include <cell/detail/web_document.hpp>
#include <cell/web_parameter_bridge.hpp>
#if defined(CELL_AUV3_DESIGNER_BINDINGS)
#include "cell_auv3_web_bindings.hpp"
#endif
#if defined(CELL_AUV3_EDITOR_METERS)
#include <cell/web_meter.hpp>
#endif
#include <mach/mach_time.h>

AUAudioUnit* cellCreateAUv3Unit(AudioComponentDescription description, NSError** error);
#if defined(CELL_AUV3_EDITOR_METERS)
cell::web::OutputMeter* cellAUv3EditorMeter(AUAudioUnit* unit);
#endif

namespace {
using cell::web::ParameterBridge;

NSString* webString(std::string_view text) {
    return [[NSString alloc] initWithBytes:text.data() length:text.size() encoding:NSUTF8StringEncoding];
}

void automate(AUParameter* parameter, AUValue value, AUParameterAutomationEventType kind, std::uint64_t time) {
    [parameter setValue:value originator:nullptr atHostTime:time eventType:kind];
}

std::vector<cell::ParamId> parameterIds([[maybe_unused]] NSArray<NSString*>* keys,
                                      std::span<const cell::Parameter> fields) {
#if defined(CELL_AUV3_DESIGNER_BINDINGS)
    for (const auto& control : cell::designer::controls) {
        if (std::none_of(fields.begin(), fields.end(), [&](const cell::Parameter& field) {
                return field.id == control.parameter && field.key == control.key;
            })) throw std::invalid_argument("Unknown AUv3 Designer control binding");
    }
    return {cell::designer::parameters.begin(), cell::designer::parameters.end()};
#else
    if (keys.count > 128) throw std::invalid_argument("Too many AUv3 editor parameter keys");
    std::vector<cell::ParamId> ids;
    ids.reserve(keys.count);
    for (NSString* key in keys) {
        const auto* utf8 = key.UTF8String;
        if (!utf8) throw std::invalid_argument("Invalid AUv3 editor parameter key");
        const auto id = cell::stableId(utf8);
        const auto field = std::find_if(fields.begin(), fields.end(),
            [id, utf8](const cell::Parameter& value) { return value.id == id && value.key == utf8; });
        if (field == fields.end())
            throw std::invalid_argument("Unknown AUv3 editor parameter key");
        ids.push_back(id);
    }
    return ids;
#endif
}

std::unique_ptr<ParameterBridge> makeBridge(AUAudioUnit* unit, NSArray<NSString*>* keys) {
    const auto fields = cell::plugin::descriptor().parameters;
    const auto ids = parameterIds(keys, fields);
    if (ids.empty()) return nullptr;
    auto* tree = unit.parameterTree;
    if (!tree || std::any_of(ids.begin(), ids.end(), [&](cell::ParamId id) { return ![tree parameterWithAddress:id]; }))
        throw std::invalid_argument("AUv3 parameter tree does not expose an editor binding");
    return std::make_unique<ParameterBridge>(ParameterBridge::Endpoint{
        fields,
        [tree](cell::ParamId id) {
            return static_cast<double>([tree parameterWithAddress:id].value);
        },
        [tree](cell::ParamId id) {
            auto* parameter = [tree parameterWithAddress:id];
            automate(parameter, parameter.value, AUParameterAutomationEventTypeTouch, mach_absolute_time());
            return true;
        },
        [tree](cell::ParamId id, double value, bool active) {
            auto* parameter = [tree parameterWithAddress:id];
            const auto time = mach_absolute_time();
            if (!active) automate(parameter, parameter.value, AUParameterAutomationEventTypeTouch, time);
            automate(parameter, static_cast<AUValue>(value), AUParameterAutomationEventTypeValue, time);
            if (!active) automate(parameter, parameter.value, AUParameterAutomationEventTypeRelease, time);
            return true;
        },
        [tree](cell::ParamId id) {
            auto* parameter = [tree parameterWithAddress:id];
            automate(parameter, parameter.value, AUParameterAutomationEventTypeRelease, mach_absolute_time());
            return true;
        }
    }, ids
#if defined(CELL_AUV3_DESIGNER_BINDINGS) && defined(CELL_DESIGNER_HAS_RULE_SOURCES)
        , std::span<const cell::web::ActionBinding>{}, cell::designer::readOnlyParameters
#endif
    );
}

NSString* bundledEditor(NSBundle* bundle) {
    NSURL* url = [bundle URLForResource:@"CellEditor" withExtension:@"html"];
    NSNumber* size = nil;
    if (![url getResourceValue:&size forKey:NSURLFileSizeKey error:nil] ||
        !size.unsignedLongLongValue || size.unsignedLongLongValue > cell::web::detail::maxHtmlBytes) return nil;
    NSData* data = [NSData dataWithContentsOfURL:url options:NSDataReadingMappedIfSafe error:nil];
    if (!data.length || data.length > cell::web::detail::maxHtmlBytes) return nil;
    const std::string_view text(static_cast<const char*>(data.bytes), data.length);
    if (text.find('\0') != text.npos) return nil;
    NSString* html = webString(text);
    return html ? [webString(cell::web::detail::offlineContentPolicy) stringByAppendingString:html] : nil;
}
} // namespace

@interface CellAUv3MessageHandler : NSObject<WKScriptMessageHandler>
@property(nonatomic, weak) id<WKScriptMessageHandler> target;
@end

@implementation CellAUv3MessageHandler
- (void)userContentController:(WKUserContentController*)controller didReceiveScriptMessage:(WKScriptMessage*)message {
    [self.target userContentController:controller didReceiveScriptMessage:message];
}
@end

@interface CellAUv3Editor : AUViewController<AUAudioUnitFactory, WKNavigationDelegate, WKScriptMessageHandler>
@end

@implementation CellAUv3Editor {
    WKWebView* _web;
    WKNavigation* _navigation;
    NSTimer* _refreshTimer;
    AUAudioUnit* _unit;
    std::unique_ptr<ParameterBridge> _bridge;
    std::string _lastSnapshot;
#if defined(CELL_AUV3_EDITOR_METERS)
    cell::web::OutputMeter* _meter;
    std::string _lastMeters;
#endif
    BOOL _loaded;
    BOOL _acceptMessages;
}

- (AUAudioUnit*)createAudioUnitWithComponentDescription:(AudioComponentDescription)description error:(NSError**)error {
    AUAudioUnit* unit = cellCreateAUv3Unit(description, error);
    if (unit) {
        __weak CellAUv3Editor* editor = self;
        dispatch_async(dispatch_get_main_queue(), ^{ [editor bind:unit]; });
    }
    return unit;
}

- (void)bind:(AUAudioUnit*)unit {
    if (_unit == unit) return;
    _bridge.reset();
#if defined(CELL_AUV3_EDITOR_METERS)
    if (_meter) _meter->setEnabled(false);
    _meter = cellAUv3EditorMeter(unit);
    if (_meter) _meter->setEnabled(_acceptMessages);
    _lastMeters.clear();
#endif
    _unit = unit;
    _lastSnapshot.clear();
    if (_web) [self bindParameters];
}

- (NSBundle*)editorBundle { return [NSBundle bundleForClass:CellAUv3Editor.class]; }

- (void)bindParameters {
    if (!_unit) return;
    try {
        NSArray<NSString*>* keys = [self editorBundle].infoDictionary[@"CellEditorParameters"];
        _bridge = makeBridge(_unit, keys);
        [self refresh];
    } catch (const std::exception&) {
        [self showEditorError:@"The editor’s parameter configuration is unavailable."];
    }
}

- (void)loadView {
    WKWebViewConfiguration* config = [[WKWebViewConfiguration alloc] init];
    config.websiteDataStore = WKWebsiteDataStore.nonPersistentDataStore;
    CellAUv3MessageHandler* handler = [[CellAUv3MessageHandler alloc] init];
    handler.target = self;
    [config.userContentController addScriptMessageHandler:handler name:@"cell"];
    WKUserScript* script = [[WKUserScript alloc] initWithSource:webString(cell::web::detail::bridgeScript())
        injectionTime:WKUserScriptInjectionTimeAtDocumentStart forMainFrameOnly:YES];
    [config.userContentController addUserScript:script];

    const auto& descriptor = cell::plugin::descriptor();
    const CGRect frame = CGRectMake(0, 0, descriptor.editorWidth, descriptor.editorHeight);
    self.preferredContentSize = frame.size;
    _web = [[WKWebView alloc] initWithFrame:frame configuration:config];
    _web.navigationDelegate = self;
    self.view = _web;

    NSString* html = bundledEditor([self editorBundle]);
    if (!html) {
        [self showEditorError:@"The bundled editor could not be opened."];
        return;
    }
    _navigation = [_web loadHTMLString:html baseURL:nil];
    [self bindParameters];
}

- (void)showEditorError:(NSString*)text {
    [self pauseRefreshing];
    _loaded = NO;
    _navigation = nil;
    _bridge.reset();
    // The caller supplies fixed application strings only.
    NSString* content = [NSString stringWithFormat:
        @"<!doctype html><meta charset=utf-8><meta name=viewport content='width=device-width'>"
         "<p style='font:16px system-ui;padding:12px'>%@</p>", text];
    [_web loadHTMLString:content baseURL:nil];
}

- (void)refresh {
    if (!_loaded) return;
    try {
        if (_bridge) [self sendSnapshot:_bridge->snapshot() last:_lastSnapshot];
#if defined(CELL_AUV3_EDITOR_METERS)
        if (_meter) [self sendSnapshot:_meter->snapshot() last:_lastMeters];
#endif
    } catch (const std::exception&) {
        [self showEditorError:@"The editor’s parameter snapshot is unavailable."];
    }
}

- (void)sendSnapshot:(std::string)snapshot last:(std::string&)last {
    if (snapshot == last) return;
    [_web evaluateJavaScript:webString("window.__cellReceive(" + snapshot + ");") completionHandler:nil];
    last = std::move(snapshot);
}

- (void)startRefreshing {
    if (_refreshTimer) return;
    _acceptMessages = YES;
#if defined(CELL_AUV3_EDITOR_METERS)
    if (_meter) _meter->setEnabled(true);
#endif
    __weak CellAUv3Editor* editor = self;
    _refreshTimer = [NSTimer timerWithTimeInterval:1.0 / 30 repeats:YES block:^(NSTimer*) { [editor refresh]; }];
    [NSRunLoop.mainRunLoop addTimer:_refreshTimer forMode:NSRunLoopCommonModes];
    [self refresh];
}

- (void)pauseRefreshing {
    _acceptMessages = NO;
#if defined(CELL_AUV3_EDITOR_METERS)
    if (_meter) _meter->setEnabled(false);
#endif
    [_refreshTimer invalidate];
    _refreshTimer = nil;
    if (_bridge) _bridge->cancel();
}

#if TARGET_OS_IPHONE
- (void)viewDidAppear:(BOOL)animated {
    [super viewDidAppear:animated];
    [self startRefreshing];
}
- (void)viewWillDisappear:(BOOL)animated {
    [self pauseRefreshing];
    [super viewWillDisappear:animated];
}
#else
- (void)viewDidAppear {
    [super viewDidAppear];
    [self startRefreshing];
}
- (void)viewWillDisappear {
    [self pauseRefreshing];
    [super viewWillDisappear];
}
#endif

- (void)webView:(WKWebView*)view decidePolicyForNavigationAction:(WKNavigationAction*)action
 decisionHandler:(void (^)(WKNavigationActionPolicy))decisionHandler {
    NSString* url = action.request.URL.absoluteString;
    const BOOL allow = view == _web && action.targetFrame.isMainFrame &&
        ([url isEqualToString:@"about:blank"] || [url hasPrefix:@"about:blank#"]);
    decisionHandler(allow ? WKNavigationActionPolicyAllow : WKNavigationActionPolicyCancel);
}

- (void)webView:(WKWebView*)view didFinishNavigation:(WKNavigation*)navigation {
    if (view != _web || navigation != _navigation) return;
    _navigation = nil;
    _loaded = YES;
    _lastSnapshot.clear();
#if defined(CELL_AUV3_EDITOR_METERS)
    _lastMeters.clear();
#endif
    [self refresh];
}

- (void)webView:(WKWebView*)view didFailProvisionalNavigation:(WKNavigation*)navigation withError:(NSError*)error {
    (void)error;
    if (view == _web && navigation == _navigation) [self showEditorError:@"The bundled editor could not be loaded."];
}

- (void)webView:(WKWebView*)view didFailNavigation:(WKNavigation*)navigation withError:(NSError*)error {
    [self webView:view didFailProvisionalNavigation:navigation withError:error];
}

- (void)webViewWebContentProcessDidTerminate:(WKWebView*)view {
    if (view == _web) [self showEditorError:@"The editor stopped. Reopen it to reconnect."];
}

- (void)userContentController:(WKUserContentController*)controller didReceiveScriptMessage:(WKScriptMessage*)message {
    if (!_acceptMessages || !_loaded || !_bridge || controller != _web.configuration.userContentController ||
        !message.frameInfo.isMainFrame || ![message.body isKindOfClass:NSString.class]) return;
    NSString* body = message.body;
    if (body.length > cell::web::detail::maxMessageBytes) return;
    const auto bytes = [body lengthOfBytesUsingEncoding:NSUTF8StringEncoding];
    const auto* utf8 = body.UTF8String;
    if (!utf8 || bytes > cell::web::detail::maxMessageBytes) return;
    (void)_bridge->apply({utf8, bytes});
    // The page defers incoming values during a drag. Refresh after its final
    // message even if the value was already sent while the control was active.
    _lastSnapshot.clear();
}

- (void)dealloc {
    [_refreshTimer invalidate];
#if defined(CELL_AUV3_EDITOR_METERS)
    if (_meter) _meter->setEnabled(false);
#endif
    _bridge.reset();
}
@end
