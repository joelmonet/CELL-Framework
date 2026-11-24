#include "gain.hpp"
#include "page.hpp"
#include <cell/ui.hpp>
#include <cell/files.hpp>
#include <cell/webview.hpp>
#include <cell/web_parameter_bridge.hpp>
#include <chrono>
#include <filesystem>
#include <iostream>
#include <optional>
#include <thread>

namespace {
std::string readHtml(const std::filesystem::path& path) {
    const auto bytes = cell::files::readBounded(path, 8 * 1024 * 1024);
    if (bytes.empty()) throw std::runtime_error("The HTML document is empty");
    return {reinterpret_cast<const char*>(bytes.data()), bytes.size()};
}

class EditorFile {
public:
    explicit EditorFile(std::filesystem::path path) : path_(std::move(path)), loaded_(stamp()) {}

    std::optional<std::string> changed() {
        const auto now = std::chrono::steady_clock::now();
        if (now < nextCheck_) return {};
        nextCheck_ = now + std::chrono::milliseconds(250);
        const auto current = stamp();
        if (!current || loaded_ == current) {
            observed_.reset();
            return {};
        }
        if (observed_ != current) {
            observed_ = current;
            return {};
        }
        auto html = readHtml(path_);
        if (stamp() != current) {
            observed_.reset();
            return {};
        }
        loaded_ = current;
        return html;
    }

private:
    using Stamp = std::pair<std::filesystem::file_time_type, std::uintmax_t>;
    std::optional<Stamp> stamp() const {
        std::error_code error;
        const auto time = std::filesystem::last_write_time(path_, error);
        if (error) return {};
        const auto bytes = std::filesystem::file_size(path_, error);
        if (error || !bytes || bytes > 8 * 1024 * 1024) return {};
        return Stamp{time, bytes};
    }
    std::filesystem::path path_;
    std::optional<Stamp> loaded_, observed_;
    std::chrono::steady_clock::time_point nextCheck_{};
};
}

class Shell final:public cell::ui::Editor {
public:
    int width{},height{};
    void paint(cell::ui::Canvas& canvas,cell::ui::Rect bounds)override{
        canvas.fill(bounds,cell::ui::Color{0xff14272d});
        // Window input/painting is logical; browser bounds are physical pixels.
        // toNativePoint is used by main for native host coordinate conversion.
        width=int(bounds.width);height=int(bounds.height);
    }
    bool pointer(const cell::ui::PointerEvent&)override{return false;}
    bool preserveAspectRatio()const noexcept override{return false;}
    cell::ui::Point constrainSize(cell::ui::Point p)const noexcept override{
        return {std::clamp(p.x,400.,1600.),std::clamp(p.y,300.,1000.)};
    }
    const char* title()const noexcept override{return "Cell WebView example";}
};
int main(int argc,char** argv){
    try{
        std::string html(cellWebExampleHtml);
        std::optional<EditorFile> file;
        std::filesystem::path editorPath;
        std::filesystem::path profile;
        for(int index=1;index<argc;++index){
            const std::string_view argument=argv[index];
            if ((argument == "--html" || argument == "--watch-html") && index + 1 < argc && editorPath.empty()) {
                editorPath = std::filesystem::absolute(argv[++index]);
                if (argument == "--watch-html") file.emplace(editorPath);
                html = readHtml(editorPath);
            }else if(!argument.starts_with("--")&&profile.empty()){
                profile=std::filesystem::absolute(argv[index]);
            }else{
                throw std::runtime_error("Usage: cell_web_editor_example [PROFILE_DIR] [--html FILE | --watch-html FILE]");
            }
        }
        cell::Engine engine(cell::plugin::descriptor(),cell::plugin::createProcessor());
        engine.prepare(48000,64,2);
        const std::array allowed{cell::example::gainId};
        const std::array actions{cell::web::ActionBinding{cell::example::factoryRecallAction, cell::web::ActionMode::invoke}};
        cell::web::ParameterBridge bridge(engine,allowed,true,actions);
        Shell shell;cell::ui::Window window(shell);
        if(!window.open(0,760,420))throw std::runtime_error("Cannot open native window");
        cell::web::View browser;cell::web::Options options;options.parent=window.nativeHandle();options.bounds={0,0,760,420};
        // WebView2 needs an explicit application-owned absolute profile path.
        // Linux/macOS use private ephemeral sessions in this revision.
        if(!profile.empty()){options.userDataDirectory=profile;std::filesystem::create_directories(profile);}
        if(!browser.open(options))throw std::runtime_error(browser.error());
        if(!browser.loadHtml(html))throw std::runtime_error("Cannot load example HTML");
        int width=760,height=420;std::string sent;
        auto last=std::chrono::steady_clock::now();bool failed=false, loaded=false;
        while(window.isOpen()){
            window.poll();
            for(const auto& event:browser.poll()){
                if (event.kind == cell::web::Event::Kind::message && loaded) {
                    bridge.apply(event.text);
                    sent.clear();
                }
                if (event.kind == cell::web::Event::Kind::loaded) { loaded = true; sent.clear(); }
                if(event.kind==cell::web::Event::Kind::error){std::cerr<<event.text<<'\n';failed=true;window.close();}
            }
            if(!window.isOpen())break;
            if (file && loaded) {
                try {
                    if (auto next = file->changed(); next && *next != html && browser.loadHtml(*next)) {
                        bridge.cancel();
                        loaded = false;
                        html = std::move(*next);
                    }
                } catch (const std::exception& error) {
                    std::cerr << "Waiting for editor file: " << error.what() << '\n';
                }
            }
            const auto point=window.toNativePoint({double(shell.width),double(shell.height)});
            // Cocoa's native parent coordinates are points, while WebView's
            // public Bounds contract uses backing pixels.
            const double density=browser.parentPixelScale();
            const int w=int(point.x*density),h=int(point.y*density);
            if(w>0&&h>0&&(w!=width||h!=height)){browser.setBounds({0,0,w,h});width=w;height=h;}
            cell::ParameterEdit edit;while(engine.pollEdit(edit)){} // no plugin host in this UI-only demo
            if(loaded&&browser.ready()&&std::chrono::steady_clock::now()-last>std::chrono::milliseconds(50)){
                const auto snapshot=bridge.snapshot();if(snapshot!=sent&&browser.send(snapshot))sent=snapshot;
                last=std::chrono::steady_clock::now();
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(8));
        }
        bridge.cancel();browser.close();
        // This is an executable, not a dynamically unloadable plugin. Keep its
        // GUI loop alive until asynchronous browser creation callbacks settle.
        while(!cell::web::View::readyToUnload()){window.poll();std::this_thread::sleep_for(std::chrono::milliseconds(8));}
        return failed?1:0;
    }catch(const std::exception& error){std::cerr<<error.what()<<'\n';return 1;}
}
