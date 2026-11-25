// SPDX-License-Identifier: Apache-2.0
#include <clap/clap.h>
#include <X11/Xlib.h>
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
    const auto window = XCreateSimpleWindow(display, DefaultRootWindow(display), 0, 0, 640, 480, 0, 0, 0);
    XMapWindow(display, window);
    XSync(display, False);
    auto* library = dlopen(argv[1], RTLD_NOW | RTLD_LOCAL);
    if (!library) throw std::runtime_error(dlerror());
    const auto* entry = static_cast<const clap_plugin_entry_t*>(dlsym(library, "clap_entry"));
    check(entry && entry->init(argv[1]), "Could not initialize the native editor fixture");
    const auto* factory = static_cast<const clap_plugin_factory_t*>(entry->get_factory(CLAP_PLUGIN_FACTORY_ID));
    std::atomic<bool> pending{};
    const clap_host_t host{
        CLAP_VERSION, &pending, "Cell native smoke host", "Cell", "", "1",
        [](const clap_host_t*, const char*) -> const void* { return nullptr; },
        [](const clap_host_t*) {}, [](const clap_host_t*) {},
        [](const clap_host_t* host) { static_cast<std::atomic<bool>*>(host->host_data)->store(true); }};
    const auto* plugin = factory->create_plugin(factory, &host, factory->get_plugin_descriptor(factory, 0)->id);
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
    bool passed{};
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(25);
    while (!passed && std::chrono::steady_clock::now() < deadline) {
        if (pending.exchange(false)) plugin->on_main_thread(plugin);
        double value{};
        passed = params->get_value(plugin, lane.id, &value) && value == 2.0;
        std::this_thread::sleep_for(std::chrono::milliseconds(4));
    }
    gui->destroy(plugin);
    plugin->destroy(plugin);
    entry->deinit();
    dlclose(library);
    XDestroyWindow(display, window);
    XCloseDisplay(display);
    check(passed, "Editor snapshot, offline asset, file dialog or program recall did not complete");
    std::cout << "PASS timer-free CLAP editor, offline assets, file dialog and program import and recall\n";
} catch (const std::exception& error) {
    std::cerr << error.what() << '\n';
    return 1;
}
