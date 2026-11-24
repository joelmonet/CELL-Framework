// Original Cell Framework implementation; see LICENSE.
#import <Cocoa/Cocoa.h>
#import <WebKit/WebKit.h>
#include <clap/clap.h>
#include <chrono>
#include <cstring>
#include <dlfcn.h>
#include <iostream>
#include <stdexcept>
#include <vector>

namespace {
void check(bool condition, const char* message) {
    if (!condition) throw std::runtime_error(message);
}

struct Host {
    unsigned flushes{};
    const clap_host_t api{CLAP_VERSION, this, "Cell Cocoa editor check", "Cell", "", "1",
        [](const clap_host_t*, const char* id) -> const void* {
            static const clap_host_params_t params{nullptr, nullptr, [](const clap_host_t* host) {
                ++static_cast<Host*>(host->host_data)->flushes;
            }};
            static const clap_host_log_t log{[](const clap_host_t*, clap_log_severity, const char* message) {
                std::cerr << message << '\n';
            }};
            if (std::strcmp(id, CLAP_EXT_PARAMS) == 0) return &params;
            if (std::strcmp(id, CLAP_EXT_LOG) == 0) return &log;
            return nullptr;
        },
        [](const clap_host_t*) {}, [](const clap_host_t*) {}, [](const clap_host_t*) {}};
};

struct Events {
    clap_event_param_value_t input{};
    std::vector<unsigned> types;
    std::vector<double> values;
    clap_input_events_t in{this, [](const clap_input_events_t* list) {
        return static_cast<Events*>(list->ctx)->input.header.size ? 1u : 0u;
    }, [](const clap_input_events_t* list, unsigned) -> const clap_event_header_t* {
        return &static_cast<Events*>(list->ctx)->input.header;
    }};
    clap_output_events_t out{this, [](const clap_output_events_t* list, const clap_event_header_t* event) {
        auto& events = *static_cast<Events*>(list->ctx);
        events.types.push_back(event->type);
        if (event->type == CLAP_EVENT_PARAM_VALUE)
            events.values.push_back(reinterpret_cast<const clap_event_param_value_t*>(event)->value);
        return true;
    }};
};

struct Plugin {
    void* module{};
    const clap_plugin_entry_t* entry{};
    const clap_plugin_t* plugin{};
    const clap_plugin_gui_t* gui{};
    const clap_plugin_params_t* params{};

    Plugin(const char* executable, Host& host) {
        module = dlopen(executable, RTLD_NOW | RTLD_LOCAL);
        if (!module) throw std::runtime_error(dlerror());
        entry = static_cast<const clap_plugin_entry_t*>(dlsym(module, "clap_entry"));
        check(entry && entry->init(executable), "Load the CLAP entry");
        const auto* factory = static_cast<const clap_plugin_factory_t*>(entry->get_factory(CLAP_PLUGIN_FACTORY_ID));
        check(factory && factory->get_plugin_count(factory) == 1, "Find the gain processor");
        plugin = factory->create_plugin(factory, &host.api, factory->get_plugin_descriptor(factory, 0)->id);
        check(plugin && plugin->init(plugin), "Initialize gain");
        gui = static_cast<const clap_plugin_gui_t*>(plugin->get_extension(plugin, CLAP_EXT_GUI));
        params = static_cast<const clap_plugin_params_t*>(plugin->get_extension(plugin, CLAP_EXT_PARAMS));
        check(gui && params, "Find GUI and parameter extensions");
    }

    ~Plugin() {
        plugin->destroy(plugin);
        entry->deinit();
        dlclose(module);
    }
};

void pump(double seconds) {
    @autoreleasepool {
        CFRunLoopRunInMode(kCFRunLoopDefaultMode, seconds, false);
    }
}

WKWebView* embeddedView(NSView* parent) {
    for (NSView* child in parent.subviews)
        if ([child isKindOfClass:[WKWebView class]]) return static_cast<WKWebView*>(child);
    return nil;
}

void editFader(WKWebView* browser, Host& host, int from, int to, bool finish) {
    const auto previous = host.flushes;
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(8);
    NSString* const script = [NSString stringWithFormat:
        @"(() => { const controls = document.querySelectorAll('input[type=range]');"
         "const fader = document.querySelector('.fader input[type=range]');"
         "if (window.__cellTestEdited || !fader || fader.disabled || controls.length !== 2 ||"
         "Array.from(controls).some(control => control.valueAsNumber !== %d)) return;"
         "window.__cellTestEdited = true; fader.value = %d;"
         "fader.dispatchEvent(new Event('input')); %@ })()",
        from, to, finish ? @"fader.dispatchEvent(new Event('change'));" : @""];
    while (host.flushes == previous && std::chrono::steady_clock::now() < deadline) {
        [browser evaluateJavaScript:script completionHandler:nil];
        pump(.1);
    }
    check(host.flushes > previous, "Designer fader did not reach the host");
}

void awaitEdits(Plugin& plugin, Events& events, std::size_t expected) {
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
    do {
        plugin.params->flush(plugin.plugin, &events.in, &events.out);
        if (events.types.size() >= expected) return;
        pump(.025);
    } while (std::chrono::steady_clock::now() < deadline);
    throw std::runtime_error("Timed out waiting for the CLAP gesture events");
}
}

int main(int argc, char** argv) {
    @autoreleasepool {
        try {
            check(argc == 2, "Pass the Cell Gain Example .clap bundle path");
            auto* path = [NSString stringWithUTF8String:argv[1]];
            check(path != nil, "Read the UTF-8 bundle path");
            if (![path isAbsolutePath]) {
                path = [[[NSFileManager defaultManager] currentDirectoryPath] stringByAppendingPathComponent:path];
            }
            const auto* executable = [[NSBundle bundleWithPath:path] executablePath].fileSystemRepresentation;
            check(executable != nullptr, "Find the executable in the CLAP bundle");

            [NSApplication sharedApplication];
            [NSApp setActivationPolicy:NSApplicationActivationPolicyAccessory];
            [NSApp finishLaunching];
            const auto frame = NSMakeRect(0, 0, 760, 540);
            auto* window = [[NSWindow alloc] initWithContentRect:frame styleMask:NSWindowStyleMaskTitled
                backing:NSBackingStoreBuffered defer:NO];
            window.releasedWhenClosed = NO;
            window.title = @"Cell Cocoa editor check";
            auto* parent = [[[NSView alloc] initWithFrame:frame] autorelease];
            [parent setBoundsOrigin:NSMakePoint(14, 17)];
            window.contentView = parent;
            [window orderFrontRegardless];

            Host host;
            clap_window_t anchor{};
            anchor.api = CLAP_WINDOW_API_COCOA;
            anchor.cocoa = parent;
            {
                Plugin p(executable, host);
                const char* api{};
                bool floating = true;
                check(p.gui->get_preferred_api(p.plugin, &api, &floating) &&
                      !floating && std::strcmp(api, CLAP_WINDOW_API_COCOA) == 0, "Choose Cocoa embedding");
                check(p.gui->create(p.plugin, CLAP_WINDOW_API_COCOA, false) &&
                      p.gui->set_parent(p.plugin, &anchor) && p.gui->show(p.plugin), "Show the editor without a host timer");
                check(p.gui->hide(p.plugin) && embeddedView(parent) == nil, "Close during browser startup");
                p.gui->destroy(p.plugin);
            }
            pump(.25);
            auto* retained = dlopen(executable, RTLD_NOLOAD | RTLD_LAZY | RTLD_LOCAL);
            check(retained != nullptr, "Keep callbacks mapped after the host closes its bundle handle");
            dlclose(retained);
            {
                Plugin p(executable, host);
                check(p.gui->create(p.plugin, CLAP_WINDOW_API_COCOA, false) &&
                      p.gui->set_parent(p.plugin, &anchor) && p.gui->show(p.plugin), "Reopen the editor");
                auto* browser = embeddedView(parent);
                check(browser != nil, "Find the embedded WebKit view");
                check(p.gui->set_size(p.plugin, 485, 310) &&
                      NSEqualRects(browser.frame, NSMakeRect(NSMinX(parent.bounds), NSMaxY(parent.bounds) - 310, 485, 310)),
                      "Resize in Cocoa points relative to the parent bounds");
                editFader(browser, host, 0, -12, true);
                Events events;
                awaitEdits(p, events, 3);
                check(events.types == std::vector<unsigned>{CLAP_EVENT_PARAM_GESTURE_BEGIN, CLAP_EVENT_PARAM_VALUE,
                                                           CLAP_EVENT_PARAM_GESTURE_END} &&
                      events.values == std::vector<double>{-12}, "Deliver a balanced gesture and gain to CLAP");
                check(p.gui->hide(p.plugin) && embeddedView(parent) == nil, "Detach the first editor");

                clap_param_info_t gain{};
                check(p.params->get_info(p.plugin, 0, &gain), "Read the gain parameter ID");
                events.input.header = {sizeof(events.input), 0, CLAP_CORE_EVENT_SPACE_ID, CLAP_EVENT_PARAM_VALUE, 0};
                events.input.param_id = gain.id;
                events.input.note_id = events.input.key = events.input.channel = events.input.port_index = -1;
                events.input.value = -6;
                p.params->flush(p.plugin, &events.in, &events.out);
                events.input.header.size = 0;
                check(p.gui->show(p.plugin), "Show the editor again");
                browser = embeddedView(parent);
                check(browser != nil, "Find the reopened WebKit view");
                events.types.clear();
                events.values.clear();
                editFader(browser, host, -6, -9, false);
                awaitEdits(p, events, 2);
                p.gui->destroy(p.plugin);
                p.params->flush(p.plugin, &events.in, &events.out);
                check(events.types == std::vector<unsigned>{CLAP_EVENT_PARAM_GESTURE_BEGIN, CLAP_EVENT_PARAM_VALUE,
                                                           CLAP_EVENT_PARAM_GESTURE_END} &&
                      events.values == std::vector<double>{-9}, "Reopen with host state and cancel the unfinished gesture");
                check(embeddedView(parent) == nil, "Remove the WebKit child on destroy");
            }
            pump(.25);
            [window close];
            [window release];
            std::cout << "PASS: Cocoa points, two-way Designer gestures, early close and reopen without a CLAP host timer\n";
        } catch (const std::exception& error) {
            std::cerr << error.what() << '\n';
            return 1;
        }
    }
    return 0;
}
