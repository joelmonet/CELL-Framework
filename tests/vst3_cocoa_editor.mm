// Original Cell Framework implementation; see LICENSE.
#import <Cocoa/Cocoa.h>
#import <WebKit/WebKit.h>
#include "public.sdk/source/vst/hosting/hostclasses.h"
#include "public.sdk/source/vst/hosting/module.h"
#include "pluginterfaces/base/funknownimpl.h"
#include "pluginterfaces/gui/iplugview.h"
#include "pluginterfaces/vst/ivstaudioprocessor.h"
#include "pluginterfaces/vst/ivsteditcontroller.h"
#include "pluginterfaces/vst/ivstmessage.h"
#include <array>
#include <atomic>
#include <chrono>
#include <cmath>
#include <filesystem>
#include <iostream>
#include <stdexcept>
#include <thread>
#include <vector>

namespace {
using namespace Steinberg;
using namespace Steinberg::Vst;
using VST3::Hosting::Module;
using namespace std::chrono_literals;

void check(bool condition, const char* message) {
    if (!condition) throw std::runtime_error(message);
}

struct EditHandler final : U::Implements<U::Directly<IComponentHandler>> {
    std::vector<int> events;
    double value{};
    tresult PLUGIN_API beginEdit(ParamID) override { events.push_back(1); return kResultOk; }
    tresult PLUGIN_API performEdit(ParamID, ParamValue v) override { value = v; events.push_back(2); return kResultOk; }
    tresult PLUGIN_API endEdit(ParamID) override { events.push_back(3); return kResultOk; }
    tresult PLUGIN_API restartComponent(int32) override { return kResultOk; }
};

struct Effect {
    explicit Effect(const std::string& path) {
        std::string error;
        module = Module::create(path, error);
        if (!module) throw std::runtime_error(error);
        const auto& factory = module->getFactory();
        for (const auto& info : factory.classInfos()) {
            if (info.category() == kVstAudioEffectClass) component = factory.createInstance<IComponent>(info.ID());
            if (info.category() == kVstComponentControllerClass)
                controller = factory.createInstance<IEditController>(info.ID());
        }
        check(component && controller && component->initialize(host) == kResultOk &&
              controller->initialize(host) == kResultOk && controller->setComponentHandler(edits) == kResultOk,
              "Initialize the VST3 component and controller");
        auto processorPeer = FUnknownPtr<IConnectionPoint>(component);
        auto editorPeer = FUnknownPtr<IConnectionPoint>(controller);
        check(processorPeer && editorPeer && processorPeer->connect(editorPeer) == kResultOk &&
              editorPeer->connect(processorPeer) == kResultOk, "Connect the processor to the editor");
    }

    ~Effect() {
        {
            auto processorPeer = FUnknownPtr<IConnectionPoint>(component);
            auto editorPeer = FUnknownPtr<IConnectionPoint>(controller);
            processorPeer->disconnect(editorPeer);
            editorPeer->disconnect(processorPeer);
        }
        controller->setComponentHandler(nullptr);
        controller->terminate();
        component->terminate();
    }

    Module::Ptr module;
    IPtr<HostApplication> host = owned(new HostApplication);
    IPtr<EditHandler> edits = owned(new EditHandler);
    IPtr<IComponent> component;
    IPtr<IEditController> controller;
};

class MeterSignal {
public:
    explicit MeterSignal(IComponent* component) : component_(component), processor_(component) {
        ProcessSetup setup{kRealtime, kSample32, 256, 48000};
        check(processor_ && processor_->setupProcessing(setup) == kResultOk &&
              component_->setActive(true) == kResultOk, "Prepare meter test audio");
        worker_ = std::thread([this] { process(); });
    }
    ~MeterSignal() {
        finished_.store(true);
        worker_.join();
        component_->setActive(false);
    }
private:
    void process() {
        if (processor_->setProcessing(true) != kResultOk) return;
        std::array<float, 256> signal{}, left{}, right{};
        signal.fill(.5f);
        float* in[]{signal.data(), signal.data()};
        float* out[]{left.data(), right.data()};
        AudioBusBuffers inputs{}, outputs{};
        inputs.numChannels = outputs.numChannels = 2;
        inputs.channelBuffers32 = in; outputs.channelBuffers32 = out;
        ProcessData data{};
        data.processMode = kRealtime;
        data.symbolicSampleSize = kSample32;
        data.numSamples = 256;
        data.numInputs = data.numOutputs = 1;
        data.inputs = &inputs; data.outputs = &outputs;
        while (!finished_.load()) {
            processor_->process(data);
            std::this_thread::sleep_for(5ms);
        }
        processor_->setProcessing(false);
    }
    IPtr<IComponent> component_;
    FUnknownPtr<IAudioProcessor> processor_;
    std::atomic<bool> finished_{false};
    std::thread worker_;
};

WKWebView* embeddedView(NSView* parent) {
    for (NSView* child in parent.subviews)
        if ([child isKindOfClass:[WKWebView class]]) return static_cast<WKWebView*>(child);
    return nil;
}

void editFader(WKWebView* browser, EditHandler& host, int before, int after, bool checkMeters, bool end) {
    NSString* const script = [NSString stringWithFormat:
        @"(() => { const fader = document.querySelector('.fader input[type=range]');"
         "const controls = document.querySelectorAll('input[type=range]');"
         "if (window.__cellTestEdited || !fader || fader.disabled || controls.length !== 2 ||"
         "Array.from(controls).some(control => control.valueAsNumber !== %d)) return;"
         "%@ window.__cellTestEdited = true; fader.value = %d;"
         "fader.dispatchEvent(new Event('input')); %@ })()",
        before, checkMeters ? @"const meters = Array.from(document.querySelectorAll('meter'));"
            "if (meters.length !== 2 || meters.some(meter => Math.abs(meter.value + 6) > .1)) return;" : @"",
        after, end ? @"fader.dispatchEvent(new Event('change'));" : @""];
    const auto deadline = std::chrono::steady_clock::now() + 8s;
    const std::size_t events = end ? 3 : 2;
    while (host.events.size() < events && std::chrono::steady_clock::now() < deadline) {
        @autoreleasepool {
            [browser evaluateJavaScript:script completionHandler:nil];
            CFRunLoopRunInMode(kCFRunLoopDefaultMode, .08, false);
        }
    }
    check(host.events.size() == events, "Expected VST3 gesture did not arrive from the showcase editor");
}
}

int main(int argc, char** argv) {
    @autoreleasepool {
        try {
            check(argc == 2, "Pass the Cell Gain Example .vst3 bundle path");
            const auto path = std::filesystem::absolute(argv[1]).string();
            [NSApplication sharedApplication];
            [NSApp setActivationPolicy:NSApplicationActivationPolicyAccessory];
            [NSApp finishLaunching];
            const auto frame = NSMakeRect(0, 0, 760, 540);
            auto* window = [[[NSWindow alloc] initWithContentRect:frame styleMask:NSWindowStyleMaskTitled
                backing:NSBackingStoreBuffered defer:NO] autorelease];
            window.releasedWhenClosed = NO;
            window.title = @"Cell VST3 Cocoa editor check";
            auto* parent = [[[NSView alloc] initWithFrame:frame] autorelease];
            window.contentView = parent;
            [parent setBoundsOrigin:NSMakePoint(14, 17)];
            [window orderFrontRegardless];

            {
                Effect fx(path);
                auto view = owned(fx.controller->createView(ViewType::kEditor));
                check(view && view->isPlatformTypeSupported(kPlatformTypeNSView) == kResultTrue &&
                      view->isPlatformTypeSupported(kPlatformTypeHWND) == kResultFalse, "Advertise Cocoa embedding");
                check(view->attached(parent, kPlatformTypeNSView) == kResultOk &&
                      view->removed() == kResultOk && !embeddedView(parent), "Close during browser startup");
            }
            CFRunLoopRunInMode(kCFRunLoopDefaultMode, .25, false);
            {
                Effect fx(path);
                auto view = owned(fx.controller->createView(ViewType::kEditor));
                check(view && view->attached(parent, kPlatformTypeNSView) == kResultOk, "Reopen on the Cocoa run loop");
                auto* browser = embeddedView(parent);
                ViewRect size{0, 0, 485, 310};
                check(view->onSize(&size) == kResultOk && browser &&
                      NSEqualRects(browser.frame, NSMakeRect(NSMinX(parent.bounds), NSMaxY(parent.bounds) - 310, 485, 310)),
                      "Resize within the parent in Cocoa points");
                {
                    MeterSignal signal(fx.component);
                    editFader(browser, *fx.edits, 0, -12, true, true);
                }
                check(fx.edits->events == std::vector<int>{1, 2, 3} &&
                      std::abs(fx.edits->value - 48. / 72.) < 1e-6, "Balanced gesture and normalized gain");
                check(view->removed() == kResultOk && !embeddedView(parent), "Detach WebKit");
                ParameterInfo gain{};
                check(fx.controller->getParameterInfo(0, gain) == kResultOk &&
                      fx.controller->setParamNormalized(gain.id, 54. / 72.) == kResultOk, "Change gain from the host");
                fx.edits->events.clear();
                check(view->attached(parent, kPlatformTypeNSView) == kResultOk, "Reattach the same VST3 view");
                browser = embeddedView(parent);
                check(browser != nil, "Find the reopened WebKit child");
                editFader(browser, *fx.edits, -6, -9, false, false);
                check(view->removed() == kResultOk && !embeddedView(parent) &&
                      fx.edits->events == std::vector<int>{1, 2, 3} &&
                      std::abs(fx.edits->value - 51. / 72.) < 1e-6, "Cancel the active gesture on detach");
            }
            CFRunLoopRunInMode(kCFRunLoopDefaultMode, .25, false);
            [window close];
            std::cout << "PASS: VST3 Cocoa resize, automation gestures, output peak/RMS and close/reopen\n";
        } catch (const std::exception& error) {
            std::cerr << error.what() << '\n';
            return 1;
        }
    }
    return 0;
}
