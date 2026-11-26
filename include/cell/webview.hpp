// Original Cell Framework implementation; see LICENSE.
#pragma once
#include <cstdint>
#include <filesystem>
#include <memory>
#include <string>
#include <string_view>
#include <vector>
namespace cell::web {
enum class Backend { unavailable, webView2, wkWebView, webKitGtk };
struct Availability { Backend backend{}; bool available{}; std::string detail; };
// Parent coordinates are Cocoa points on macOS and physical pixels elsewhere.
enum class BoundsUnits { physicalPixels, parentCoordinates };
struct Bounds { int x{}, y{}, width=640, height=480; }; // top-left parent-client coordinates
// Images and fonts exposed at cell-asset://editor/<name>. The view owns the bytes.
struct Asset { std::string name; std::vector<std::uint8_t> bytes; };
struct Options {
    std::uintptr_t parent{};
    Bounds bounds;
    // Windows: a writable, application-owned absolute WebView2 profile path.
    // Linux/macOS currently always use private ephemeral sessions; this field
    // does not select persistent profiles on those backends.
    std::filesystem::path userDataDirectory;
    std::filesystem::path loaderPath; // optional explicit WebView2Loader.dll
    bool allowRemoteContent=false, developerTools=false;
    BoundsUnits boundsUnits=BoundsUnits::physicalPixels;
    std::vector<Asset> assets;
};
enum class HostKey { release, space };
struct Event {
    enum class Kind { ready, loaded, message, error, navigationBlocked };
    Kind kind{}; std::string text;
};
// Native system-browser child, independent of JUCE or the Canvas renderer.
// All calls and destruction belong to the creating GUI thread. Poll it from
// the host's GUI event loop. No operation belongs in an audio callback.
class View {
public:
    View(); ~View();
    View(const View&)=delete; View& operator=(const View&)=delete;
    static Availability availability(const Options& = {});
    bool open(Options);
    bool ready() const;
    bool loadHtml(std::string_view); // UTF-8 self-contained HTML, maximum 8 MiB
    bool navigate(std::string_view); // requires allowRemoteContent for remote URLs
    bool evaluate(std::string_view script); // GUI thread; maximum 1 MiB
    bool send(std::string_view json); // native -> cell.onMessage; maximum 64 KiB
    // Physical pixels per native parent coordinate unit (Cocoa points vs pixels).
    double parentPixelScale() const;
    // X11 window ID of a live embedded child, or zero when unavailable.
    std::uintptr_t x11Window() const;
    void setBounds(Bounds); void setVisible(bool); void focus();
    // Return keyboard focus to the embedding window or forward transport Space.
    bool sendHostKey(HostKey);
    std::vector<Event> poll();
    void close();
    std::string error() const;
    // WebView2 creation callbacks may outlive a closed view. A dynamic plugin
    // must settle them on the GUI thread before unloading this SDK's code.
    static bool readyToUnload() noexcept;
private:
    struct Impl; std::unique_ptr<Impl> impl_;
};
} // namespace cell::web
