// SPDX-License-Identifier: Apache-2.0
#include <clap/clap.h>
#include <X11/Xlib.h>
#include <X11/keysym.h>
#include <atomic>
#include <chrono>
#include <cstdlib>
#include <dlfcn.h>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <stdexcept>
#include <thread>

namespace {
void check(bool condition, const char* message) {
    if (!condition) throw std::runtime_error(message);
}

struct HostSignals {
    std::atomic<bool> pending{};
    bool rejectedBindings{}, appearanceUpdated{};
    unsigned reloads{};
};

struct FileFixture {
    std::filesystem::path directory;
    FileFixture() {
        auto pattern = (std::filesystem::temp_directory_path() / "cell-editor-file-XXXXXX").string();
        const auto* created = mkdtemp(pattern.data());
        check(created, "Could not create the file dialog fixture");
        directory = created;
        std::ofstream(directory / "demo.mstbank", std::ios::binary) << "MS";
        std::ofstream(directory / "large.mstbank", std::ios::binary) << std::string(46 * 1024, 'x');
        const auto helper = directory / "zenity";
        std::ofstream(helper) << R"(#!/bin/sh
count=0
if [ -f "$CELL_DIALOG_FIXTURE/calls" ]; then read -r count < "$CELL_DIALOG_FIXTURE/calls"; fi
count=$((count + 1))
printf '%s\n' "$count" > "$CELL_DIALOG_FIXTURE/calls"
case "$count" in
  1) exit 1 ;;
  2) printf '%s\n' "$CELL_DIALOG_FIXTURE/large.mstbank" ;;
  3) printf '%s\n' "$CELL_DIALOG_FIXTURE/demo.mstbank" ;;
  *) exit 1 ;;
esac
)";
        std::filesystem::permissions(helper, std::filesystem::perms::owner_all);
        const auto* path = std::getenv("PATH");
        const auto testPath = directory.string() + ":" + (path ? path : "");
        check(setenv("PATH", testPath.c_str(), 1) == 0 && setenv("CELL_DIALOG_FIXTURE", directory.c_str(), 1) == 0,
              "Could not inject the file dialog fixture");
    }
    ~FileFixture() {
        std::error_code error;
        std::filesystem::remove_all(directory,error);
    }
};
}

int main(int argc, char** argv) try {
    if (argc != 2) return 2;
    auto* display = XOpenDisplay(nullptr);
    if (!display) return 77;
    const FileFixture fileFixture;
    const auto root = DefaultRootWindow(display);
    const auto owner = XCreateSimpleWindow(display, root, 5, 5, 640, 480, 0, 0, 0);
    const auto window = XCreateSimpleWindow(display, root, 15, 15, 640, 480, 0, 0, 0);
    XSetTransientForHint(display, window, owner);
    XSelectInput(display, owner, KeyPressMask | KeyReleaseMask);
    XMapWindow(display, owner);
    XMapWindow(display, window);
    XSync(display, False);
    auto* library = dlopen(argv[1], RTLD_NOW | RTLD_LOCAL);
    if (!library) throw std::runtime_error(dlerror());
    const auto* entry = static_cast<const clap_plugin_entry_t*>(dlsym(library, "clap_entry"));
    check(entry && entry->init(argv[1]), "Could not initialize the native editor fixture");
    const auto* factory = static_cast<const clap_plugin_factory_t*>(entry->get_factory(CLAP_PLUGIN_FACTORY_ID));
    HostSignals signals;
    const clap_host_t host{
        CLAP_VERSION, &signals, "Cell native smoke host", "Cell", "", "1",
        [](const clap_host_t*, const char* id) -> const void* {
            static const clap_host_log_t log{
                [](const clap_host_t* host, clap_log_severity, const char* message) {
                    auto& signals = *static_cast<HostSignals*>(host->host_data);
                    const std::string_view text(message);
                    if (text.find("Designer changed a compiled parameter binding") != text.npos)
                        signals.rejectedBindings = true;
                    if (text == "Designer live page reloaded") ++signals.reloads;
                    if (text == "Designer live appearance updated") signals.appearanceUpdated = true;
                    std::clog << message << '\n';
                }};
            return std::string_view(id) == CLAP_EXT_LOG ? &log : nullptr;
        },
        [](const clap_host_t*) {}, [](const clap_host_t*) {},
        [](const clap_host_t* host) { static_cast<HostSignals*>(host->host_data)->pending.store(true); }};
    const auto* descriptor = factory->get_plugin_descriptor(factory, 0);
    check(setenv("CELL_DESIGNER_LIVE_DIR", fileFixture.directory.c_str(), 1) == 0 &&
          setenv("CELL_DESIGNER_LIVE_ID", descriptor->id, 1) == 0, "Could not enable Designer live reload");
    const auto* plugin = factory->create_plugin(factory, &host, descriptor->id);
    check(plugin && plugin->init(plugin), "Could not create the native editor fixture");
    const auto* gui = static_cast<const clap_plugin_gui_t*>(plugin->get_extension(plugin, CLAP_EXT_GUI));
    const auto* params = static_cast<const clap_plugin_params_t*>(plugin->get_extension(plugin, CLAP_EXT_PARAMS));
    check(gui && params && gui->is_api_supported(plugin, CLAP_WINDOW_API_X11, false),
          "A CLAP host without timer support could not use the WebView editor");
    clap_window_t parent{};
    parent.api = CLAP_WINDOW_API_X11;
    parent.x11 = window;
    check(gui->create(plugin, parent.api, false) && gui->set_parent(plugin, &parent) && gui->show(plugin),
          "Could not show the CLAP editor without host timer support");
    clap_param_info_t lane{};
    check(params->get_info(plugin, 1, &lane), "Missing fixture parameter");
    const auto wait = [&](auto predicate, int seconds) {
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(seconds);
        do {
            if (signals.pending.exchange(false)) plugin->on_main_thread(plugin);
            if (predicate()) return true;
            std::this_thread::sleep_for(std::chrono::milliseconds(4));
        } while (std::chrono::steady_clock::now() < deadline);
        return false;
    };
    const auto currentLane = [&] {
        double value{};
        check(params->get_value(plugin, lane.id, &value), "Could not query native parameter");
        return value;
    };
    check(wait([&] { return currentLane() == 2.0; }, 25),
          "Editor snapshot, offline asset, file dialog or program recall did not complete");
    const auto publish = [&](std::string_view contents) {
        const auto candidate = fileFixture.directory / "live.tmp";
        std::ofstream(candidate, std::ios::binary) << contents;
        std::filesystem::rename(candidate, fileFixture.directory / "cell-designer-live.html");
    };
    publish(R"html(<!-- cell.designer.live.v1 {"parameters":[{"key":"newParameter","writable":true}],"actions":[]}-->
<!doctype html><html><head></head><body>Invalid bindings</body></html>)html");
    check(wait([&] { return signals.rejectedBindings; }, 5) && currentLane() == 2.0,
          "Live reload did not reject a new key while retaining the current patch");
    const std::string livePage = R"html(<!-- cell.designer.live.v1 {"parameters":[],"actions":[]}-->
<!doctype html><html><head></head><body><script>
const cellLiveAppearance = {"css":"original","fonts":"","looks":[]};
const designerPage = String(window.__cellDesignerLivePage);
let receivedPatch = false;
window.__cellApplyDesignerAppearance = appearance => {
    if (appearance.css === "replacement" && receivedPatch)
        cell.postMessage({type:"host-shortcut",key:"space",designerPage});
};
cell.postMessage({type:"recall-program",bank:0,program:3,designerPage:"stale"});
cell.onMessage(message => {
    if (message.type !== "parameters") return;
    const preserved = message.programs?.active?.program === 37 &&
        message.programs?.banks[0].programs[37].name === "Imported P37" &&
        message.parameters.some(field => field.key === "slot1" && field.value === 2);
    receivedPatch = preserved;
    cell.postMessage({type:"host-shortcut",key:preserved ? "space" : "release",designerPage});
});
cell.postMessage({type:"designer-ready",designerPage});
</script></body></html>)html";
    publish(livePage);
    check(wait([&] { return signals.reloads == 1; }, 5), "CLAP host did not accept the live page");
    bool keyPressed{}, keyReleased{};
    const auto receivedSpace = [&] {
        while (XPending(display)) {
            XEvent event{};
            XNextEvent(display, &event);
            if (event.type != KeyPress && event.type != KeyRelease) continue;
            if (event.xkey.window != owner || XLookupKeysym(&event.xkey, 0) != XK_space) continue;
            if (event.type == KeyPress) keyPressed = true;
            if (event.type == KeyRelease) keyReleased = true;
        }
        return keyPressed && keyReleased;
    };
    check(wait(receivedSpace, 7) && currentLane() == 2.0,
          "Live page lost the patch, parameter snapshot or host shortcut");
    auto replacement = livePage;
    replacement.replace(replacement.find("\"original\""), sizeof("\"original\"") - 1, "\"replacement\"");
    keyPressed = keyReleased = false;
    publish(replacement);
    check(wait([&] { return signals.appearanceUpdated && receivedSpace(); }, 5) &&
          signals.reloads == 1 && currentLane() == 2.0,
          "Live theme did not update the mounted editor and preserve its patch");
    gui->destroy(plugin);
    plugin->destroy(plugin);
    entry->deinit();
    dlclose(library);
    XDestroyWindow(display, window);
    XDestroyWindow(display, owner);
    XCloseDisplay(display);
    std::cout << "PASS CLAP editor, program recall, live page bindings and native transport Space\n";
} catch (const std::exception& error) {
    std::cerr << error.what() << '\n';
    return 1;
}
