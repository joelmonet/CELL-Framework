// Original Cell Framework implementation; see LICENSE.
#import <Cocoa/Cocoa.h>
#import <AudioToolbox/AUCocoaUIView.h>
#import <AudioToolbox/AudioToolbox.h>
#import <WebKit/WebKit.h>

#include <cell/core.hpp>
#include <chrono>
#include <iostream>
#include <stdexcept>

#ifndef CELL_TEST_AU_SUBTYPE
#define CELL_TEST_AU_SUBTYPE 0x4347616e // CGan
#endif
#ifndef CELL_TEST_AU_MANUFACTURER
#define CELL_TEST_AU_MANUFACTURER 0x43656c6c // Cell
#endif

namespace {
constexpr AudioComponentDescription gainComponent{0x61756678, CELL_TEST_AU_SUBTYPE, CELL_TEST_AU_MANUFACTURER, 0, 0};
constexpr auto gain = cell::stableId("gain");
constexpr UInt32 frames = 128;

void require(bool ok, const char* message) {
    if (!ok) throw std::runtime_error(message);
}

void check(OSStatus status, const char* operation) {
    if (status != noErr) throw std::runtime_error(std::string(operation) + " failed: " + std::to_string(status));
}

template<class Predicate>
void waitFor(Predicate ready, const char* message) {
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(8);
    do {
        if (ready()) return;
        CFRunLoopRunInMode(kCFRunLoopDefaultMode, .025, false);
    } while (std::chrono::steady_clock::now() < deadline);
    throw std::runtime_error(message);
}

struct Unit {
    AudioUnit unit{};
    double position{};

    Unit() {
        const auto component = AudioComponentFindNext(nullptr, &gainComponent);
        require(component != nullptr, "Install the Cell Gain AUv2 component");
        check(AudioComponentInstanceNew(component, &unit), "Open gain unit");
    }

    ~Unit() { AudioComponentInstanceDispose(unit); }

    void setGain(float value) {
        check(AudioUnitSetParameter(unit, gain, kAudioUnitScope_Global, 0, value, 0), "Set gain");
    }

    float getGain() {
        float value{};
        check(AudioUnitGetParameter(unit, gain, kAudioUnitScope_Global, 0, &value), "Read gain");
        return value;
    }

    void configure() {
        const AudioStreamBasicDescription format{48000, kAudioFormatLinearPCM,
            static_cast<UInt32>(kAudioFormatFlagsNativeFloatPacked) | kAudioFormatFlagIsNonInterleaved,
            sizeof(float), 1, sizeof(float), 2, 32, 0};
        for (const auto scope : {kAudioUnitScope_Input, kAudioUnitScope_Output})
            check(AudioUnitSetProperty(unit, kAudioUnitProperty_StreamFormat, scope, 0, &format, sizeof(format)),
                  "Set stereo stream format");
        const AURenderCallbackStruct input{[](void*, AudioUnitRenderActionFlags*, const AudioTimeStamp*, UInt32,
                                            UInt32 count, AudioBufferList* audio) -> OSStatus {
            for (UInt32 channel = 0; channel < audio->mNumberBuffers; ++channel)
                std::fill_n(static_cast<float*>(audio->mBuffers[channel].mData), count, .5f);
            return noErr;
        }, nullptr};
        check(AudioUnitSetProperty(unit, kAudioUnitProperty_SetRenderCallback, kAudioUnitScope_Input, 0,
                                   &input, sizeof(input)), "Connect test signal");
        check(AudioUnitInitialize(unit), "Initialize gain unit");
    }

    void render() {
        struct StereoBuffers { UInt32 count; AudioBuffer buffers[2]; };
        static_assert(offsetof(StereoBuffers, buffers) == offsetof(AudioBufferList, mBuffers));
        std::array<float, frames> left{}, right{};
        StereoBuffers output{2, {{1, sizeof(left), left.data()}, {1, sizeof(right), right.data()}}};
        AudioTimeStamp timestamp{};
        timestamp.mFlags = kAudioTimeStampSampleTimeValid;
        timestamp.mSampleTime = position;
        AudioUnitRenderActionFlags flags{};
        check(AudioUnitRender(unit, &flags, &timestamp, 0, frames, reinterpret_cast<AudioBufferList*>(&output)),
              "Render metered output");
        position += frames;
    }
};

struct Gestures {
    AUEventListenerRef listener{};
    unsigned begins{}, ends{};

    ~Gestures() { if (listener) AUListenerDispose(listener); }

    void listen(AudioUnit unit) {
        check(AUEventListenerCreate([](void* data, void*, const AudioUnitEvent* event, UInt64, AudioUnitParameterValue) {
            auto& received = *static_cast<Gestures*>(data);
            if (event->mEventType == kAudioUnitEvent_BeginParameterChangeGesture) ++received.begins;
            if (event->mEventType == kAudioUnitEvent_EndParameterChangeGesture) ++received.ends;
        }, this, CFRunLoopGetMain(), kCFRunLoopDefaultMode, .01f, .01f, &listener), "Listen for gestures");
        for (const auto kind : {kAudioUnitEvent_BeginParameterChangeGesture, kAudioUnitEvent_EndParameterChangeGesture}) {
            const AudioUnitEvent event{kind, {{unit, gain, kAudioUnitScope_Global, 0}}};
            check(AUEventListenerAddEventType(listener, this, &event), "Subscribe to gesture");
        }
    }
};

id<AUCocoaUIBase> viewFactory(AudioUnit unit) {
    UInt32 size{};
    Boolean writable{};
    check(AudioUnitGetPropertyInfo(unit, kAudioUnitProperty_CocoaUI, kAudioUnitScope_Global, 0, &size, &writable),
          "Query Cocoa editor metadata");
    require(size == sizeof(AudioUnitCocoaViewInfo) && !writable, "Expected one read-only factory entry");
    AudioUnitCocoaViewInfo info{};
    check(AudioUnitGetProperty(unit, kAudioUnitProperty_CocoaUI, kAudioUnitScope_Global, 0, &info, &size),
          "Read Cocoa editor metadata");
    NSURL* location = CFBridgingRelease(info.mCocoaAUViewBundleLocation);
    NSString* name = CFBridgingRelease(info.mCocoaAUViewClass[0]);
    Class factory = [[NSBundle bundleWithURL:location] classNamed:name];
    require(factory && [factory conformsToProtocol:@protocol(AUCocoaUIBase)], "Load the published view factory");
    return [[factory alloc] init];
}

WKWebView* embeddedBrowser(NSView* view) {
    for (NSView* child in view.subviews) {
        if ([child isKindOfClass:WKWebView.class]) return static_cast<WKWebView*>(child);
        if (auto* browser = embeddedBrowser(child)) return browser;
    }
    return nil;
}

void expectPage(WKWebView* browser, NSString* script, const char* failure) {
    struct Result { bool ready{}, pending{}; };
    const auto result = std::make_shared<Result>();
    waitFor([&] {
        if (!result->pending && !result->ready) {
            result->pending = true;
            [browser evaluateJavaScript:script completionHandler:^(id value, NSError*) {
                result->ready = [value isKindOfClass:NSNumber.class] && [value boolValue];
                result->pending = false;
            }];
        }
        return result->ready;
    }, failure);
}

void expectGain(WKWebView* browser, int value) {
    expectPage(browser, [NSString stringWithFormat:
        @"(() => { const inputs = Array.from(document.querySelectorAll('input[type=range]'));"
         "return inputs.length === 2 && inputs.every(input => !input.disabled && input.valueAsNumber === %d); })()", value],
        "Host gain did not reach the Designer knob and fader");
}

void expectMeters(WKWebView* browser, double level) {
    expectPage(browser, [NSString stringWithFormat:
        @"(() => { const meters = Array.from(document.querySelectorAll('meter'));"
         "return meters.length === 2 && meters.every(meter => Math.abs(meter.value - %f) < .12); })()", level],
        "The Designer peak and RMS did not match the rendered level");
}

void edit(WKWebView* browser, Unit& unit, int value, bool finish) {
    [browser evaluateJavaScript:[NSString stringWithFormat:
        @"(() => { const fader = document.querySelector('.fader input'); fader.value = %d;"
         "fader.dispatchEvent(new Event('input')); %@ })()", value,
        finish ? @"fader.dispatchEvent(new Event('change'));" : @""] completionHandler:nil];
    waitFor([&] { return unit.getGain() == value; }, "Designer fader did not reach Core Audio");
}
}

int main() {
    @autoreleasepool {
        try {
            [NSApplication sharedApplication];
            [NSApp setActivationPolicy:NSApplicationActivationPolicyAccessory];
            [NSApp finishLaunching];
            Unit unit;
            unit.setGain(-6);
            unit.configure();
            Gestures gestures;
            gestures.listen(unit.unit);
            auto factory = viewFactory(unit.unit);
            require(factory.interfaceVersion == 0, "Audio Unit Cocoa interface version");
            auto* window = [[NSWindow alloc] initWithContentRect:NSMakeRect(0, 0, 680, 480)
                styleMask:NSWindowStyleMaskTitled backing:NSBackingStoreBuffered defer:NO];
            window.releasedWhenClosed = NO;
            auto* first = [factory uiViewForAudioUnit:unit.unit withSize:NSMakeSize(680, 480)];
            require(first != nil, "Create the gain editor");
            window.contentView = first;
            [window orderFront:nil];
            auto* browser = embeddedBrowser(first);
            require(browser != nil, "Embed WKWebView");
            [first setFrameSize:NSMakeSize(520, 320)];
            require(NSEqualSizes(browser.frame.size, NSMakeSize(520, 320)), "Resize in Cocoa points");
            expectGain(browser, -6);
            for (int i = 0; i < 32; ++i) unit.render();
            expectMeters(browser, -12.0);
            const UInt32 bypass = 1;
            check(AudioUnitSetProperty(unit.unit, kAudioUnitProperty_BypassEffect, kAudioUnitScope_Global, 0,
                                       &bypass, sizeof(bypass)), "Enable bypass");
            unit.render();
            expectMeters(browser, -6.0);
            edit(browser, unit, -9, true);
            waitFor([&] { return gestures.begins == 1 && gestures.ends == 1; }, "Complete the first gesture");
            edit(browser, unit, -12, false);
            window.contentView = [[NSView alloc] initWithFrame:window.contentView.bounds];
            waitFor([&] { return gestures.begins == 2 && gestures.ends == 2; }, "Detaching ends the active gesture");
            unit.setGain(-3);
            auto* second = [factory uiViewForAudioUnit:unit.unit withSize:NSZeroSize];
            require(second && second != first, "Create a fresh view on reopen");
            window.contentView = second;
            expectGain(embeddedBrowser(second), -3);
            window.contentView = nil;
            [window close];
            std::cout << "PASS: AUv2 Cocoa editor, gain, gestures, meters, bypass, resize and reopen\n";
        } catch (const std::exception& error) {
            std::cerr << error.what() << '\n';
            return 1;
        }
    }
}
